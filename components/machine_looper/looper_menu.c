// M2 looper UI — a Live lane view (performance) and a Setup page
// (Sync/Src/Bars). The machine's main-menu entries point here; TR buttons
// drive punch/play from the audio engine, the encoder selects the lane.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "menusys.h"
#include "menu_types.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "looper_priv.h"

static const color_t LANE_BG   = {5, 9, 28};      // darker blue track background
static const color_t BAR_BG    = {14, 22, 52};    // subtle track region inside the bar
static const color_t COL_EMPTY = {70, 90, 140};
static const color_t COL_ARMED = {230, 170, 0};
static const color_t COL_REC   = {220, 40, 40};
static const color_t COL_PLAY  = {40, 200, 90};

static const char *state_word(int s){
    switch(s){ case LP_ARMED: return "ARM"; case LP_REC: return "REC";
               case LP_PLAY: return "PLAY"; case LP_STOP: return "STOP";
               default: return "---"; }
}
static color_t state_col(int s){
    switch(s){ case LP_ARMED: return COL_ARMED; case LP_REC: return COL_REC;
               case LP_PLAY: return COL_PLAY;  default: return COL_EMPTY; }
}

// ---- Live lane view -------------------------------------------------------
// Incremental drawing: full chrome only on state/selection change, and the
// playhead only when it moves a whole pixel. Blanking-and-repainting every
// tick was the flicker. Caches reset on entry so a fresh page repaints fully.
static uint8_t s_last_state[LP_TRACKS];
static int     s_last_ph[LP_TRACKS];
static int     s_last_sel = -1;
static bool    s_last_locked;
static int     s_last_bpm10 = -12345;

static void lanes_reset_cache(void){
    for (int i = 0; i < LP_TRACKS; i++){ s_last_state[i] = 0xFF; s_last_ph[i] = -1; }
    s_last_sel = -1; s_last_bpm10 = -12345; s_last_locked = false;
}

static int lane_y(int i){ return TFT_getfontheight() + 11 + i * 46; }
#define LANE_BX 26
#define LANE_BW (_width - 150)
#define LANE_BH 16

// full lane chrome: frame, number, empty bar outline, state word, length
static void lane_chrome(int i){
    int y = lane_y(i);
    lp_track_t *t = &lp.tr[i];
    _bg = LANE_BG;
    TFT_fillRect(0, y, _width, 44, _bg);
    // frame color by state so the recording/armed lane is unmistakable;
    // recording wins over the selection frame
    color_t frame; bool draw_frame = true;
    if (t->state == LP_REC)        frame = COL_REC;
    else if (t->state == LP_ARMED) frame = COL_ARMED;
    else if (i == lp.sel)          frame = TFT_CYAN;
    else                           draw_frame = false;
    if (draw_frame){ _fg = frame; TFT_drawRect(1, y, _width - 2, 43, _fg); }

    char buf[12];
    _fg = TFT_WHITE;
    snprintf(buf, sizeof(buf), "%d", i + 1);
    TFT_print(buf, 6, y + 6);

    _fg = (color_t){40, 60, 110};
    TFT_drawRect(LANE_BX, y + 6, LANE_BW, LANE_BH, _fg);

    _fg = state_col(t->state);
    TFT_print((char*)state_word(t->state), LANE_BX + LANE_BW + 8, y + 6);
    if (t->len > 0){
        snprintf(buf, sizeof(buf), "%lus", (unsigned long)(t->len / LP_RATE));
        _fg = TFT_LIGHTGREY;
        TFT_print(buf, _width - 40, y + 6);
    }
}

// during REC the loop length isn't known yet, so show progress toward the
// auto-stop target (bar-quantized length); otherwise show play position
static uint32_t lane_denom(lp_track_t *t){
    return (t->state == LP_REC) ? t->target : t->len;
}

// just the playhead inside the bar; no full-lane blank
static void lane_playhead(int i){
    int y = lane_y(i);
    lp_track_t *t = &lp.tr[i];
    int pw = LANE_BW - 2, bx = LANE_BX + 1, by = y + 7, bh = LANE_BH - 2;
    uint32_t denom = lane_denom(t);
    // subtle track region + a bright thin playhead line (not a solid fill)
    _bg = BAR_BG;
    TFT_fillRect(bx, by, pw, bh, _bg);
    if (denom > 0){
        int ph = (int)((uint64_t)t->pos * pw / denom);
        int lw = 3;
        if (ph > pw - lw) ph = pw - lw;
        if (ph < 0) ph = 0;
        _bg = state_col(t->state);
        TFT_fillRect(bx + ph, by, lw, bh, _bg);
        s_last_ph[i] = ph;
    } else {
        s_last_ph[i] = -1;
    }
}

static void lanes_update(void){
    for (int i = 0; i < LP_TRACKS; i++){
        lp_track_t *t = &lp.tr[i];
        bool chrome = (t->state != s_last_state[i]) || (i == lp.sel) != (i == s_last_sel);
        if (chrome){ lane_chrome(i); lane_playhead(i); s_last_state[i] = t->state; }
        else {
            int pw = LANE_BW - 2;
            uint32_t denom = lane_denom(t);
            int ph = (denom > 0) ? (int)((uint64_t)t->pos * pw / denom) : 0;
            if (ph != s_last_ph[i]) lane_playhead(i);
        }
    }
    s_last_sel = lp.sel;
}

static void header_update(void){
    int bpm10 = (int)(lp.bpm * 10.0f);
    if (bpm10 == s_last_bpm10 && lp.locked == s_last_locked) return;
    s_last_bpm10 = bpm10; s_last_locked = lp.locked;
    int fh = TFT_getfontheight();
    _bg = (color_t){10, 18, 56};
    TFT_fillRect(_width / 2, 0, _width / 2, fh + 8, _bg);
    char buf[24];
    if (lp.locked) snprintf(buf, sizeof(buf), "%.1f BPM [CLK]", lp.bpm);
    else           snprintf(buf, sizeof(buf), "-- BPM");
    _fg = lp.locked ? (color_t){40, 200, 90} : TFT_LIGHTGREY;
    TFT_print(buf, _width - TFT_getStringWidth(buf) - 6, 4);
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _bg = (color_t){10, 18, 56};
    TFT_fillRect(0, 0, _width, fh + 8, _bg);
    _fg = TFT_WHITE; TFT_print("Looper", 6, 4);
    lanes_reset_cache();
    header_update();
    lanes_update();
    // control hint along the bottom
    _bg = TFT_BLACK; _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn:select  press:rec/stop  hold:exit", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int looper_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU:
            live_full_redraw();
            break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST:
            header_update();
            lanes_update();
            break;
        case EV_FWD:
            lp.sel = (lp.sel + 1) % LP_TRACKS;
            lanes_update();
            break;
        case EV_BWD:
            lp.sel = (lp.sel + LP_TRACKS - 1) % LP_TRACKS;
            lanes_update();
            break;
        case EV_SHORT_PRESS:
            // context action on selected lane (arm/cancel/punch/play/stop)
            lp.cmd_action[lp.sel] = 1;
            break;
        case EV_LONG_PRESS:
            return M_MAIN;
        default:
            break;
    }
    return 0;
}

// ---- Setup page -----------------------------------------------------------
// Rows 0-2 are edit-in-place values; row 3 (Save) is an action button.
static const char *setup_labels[] = {"Sync", "Clock Src", "Bars", "Monitor", "Save Trk"};
#define SETUP_N 5
#define SETUP_SAVE_ROW 4
static const char *s_save_msg = "";   // transient result shown on the Save row

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE;
    TFT_print("Looper Setup", 6, 4);
    for (int i = 0; i < SETUP_N; i++){
        int y = fh + 12 + i * (fh + 8);
        _bg = (i == pos) ? (color_t){10,18,56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
        TFT_print((char*)setup_labels[i], 8, y);
        char v[24];
        switch(i){
            case 0: snprintf(v, sizeof(v), "%s", lp.sync_on ? "ON" : "OFF"); break;
            case 1:
                if (lp.clk_src == LP_CLK_TR1)      snprintf(v, sizeof(v), "TR1");
                else if (lp.clk_src == LP_CLK_TR2) snprintf(v, sizeof(v), "TR2");
                else snprintf(v, sizeof(v), "CV%d", lp.clk_src + 1);
                break;
            case 2: snprintf(v, sizeof(v), "%d", lp.bars); break;
            case 3: snprintf(v, sizeof(v), "%s", lp.monitor ? "ON" : "OFF"); break;
            case 4: snprintf(v, sizeof(v), "%s trk %d",
                             s_save_msg[0] ? s_save_msg : "press:", lp.sel + 1); break;
        }
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
}

static int looper_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; s_save_msg = ""; setup_redraw(pos, sel); break;
        case EV_FWD:
            if(sel){
                if(pos==0) lp.sync_on = !lp.sync_on;
                else if(pos==1) lp.clk_src = (lp.clk_src + 1) % LP_CLK_SRCS;
                else if(pos==2) { int b = lp.bars * 2; lp.bars = (b > 8) ? 1 : b; }
                else if(pos==3) lp.monitor = !lp.monitor;
            } else { pos = (pos + 1) % SETUP_N; s_save_msg = ""; }
            setup_redraw(pos, sel);
            break;
        case EV_BWD:
            if(sel){
                if(pos==0) lp.sync_on = !lp.sync_on;
                else if(pos==1) lp.clk_src = (lp.clk_src + LP_CLK_SRCS - 1) % LP_CLK_SRCS;
                else if(pos==2) { int b = lp.bars / 2; lp.bars = (b < 1) ? 8 : b; }
                else if(pos==3) lp.monitor = !lp.monitor;
            } else { pos = (pos + SETUP_N - 1) % SETUP_N; s_save_msg = ""; }
            setup_redraw(pos, sel);
            break;
        case EV_SHORT_PRESS:
            if(pos == SETUP_SAVE_ROW){
                // action row: write the selected track to the SD library
                int r = looper_save_track(lp.sel);
                s_save_msg = (r == 0) ? "SAVED" : "EMPTY";
                setup_redraw(pos, 0);
            } else {
                sel = !sel; setup_redraw(pos, sel);
            }
            break;
        case EV_LONG_PRESS: return M_MAIN;
        default: break;
    }
    return 0;
}

// ---- registration ---------------------------------------------------------
static void looper_register_pages(void *menusys){
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_LOOPER_LIVE);
    menusys_item_set_default_cb(_ms, M_LOOPER_LIVE, looper_live_handler);
    menusys_new_item(_ms, M_LOOPER_SETUP);
    menusys_item_set_default_cb(_ms, M_LOOPER_SETUP, looper_setup_handler);
}

// main screen: lanes as a live backdrop BELOW the menu bar. Must not draw the
// header — that row belongs to the core main menu (Live/Setup/System labels);
// painting over it left the scroll highlight sliding through blank space.
static int looper_main_event(int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU:
            lanes_reset_cache();
            lanes_update();
            break;
        case EV_TIMER_REPEATING_SLOW:
            lanes_update();
            break;
        default: break;
    }
    return 0;
}

static const char *const looper_main_items[] = {"Live", "Setup"};
static const int looper_main_targets[] = {M_LOOPER_LIVE, M_LOOPER_SETUP};

const machine_ui_t looper_menu_ui = {
    .main_items = looper_main_items,
    .main_targets = looper_main_targets,
    .n_main = 2,
    .register_pages = looper_register_pages,
    .main_event = looper_main_event,
};
