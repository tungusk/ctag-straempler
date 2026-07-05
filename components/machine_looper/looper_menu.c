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

static const color_t LANE_BG   = {10, 18, 56};
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
static void draw_lane(int i, bool full){
    int fh = TFT_getfontheight();
    int top = fh + 11;
    int lane_h = 46;
    int y = top + i * lane_h;
    lp_track_t *t = &lp.tr[i];

    // lane background; selected lane gets a bright frame
    _bg = LANE_BG;
    TFT_fillRect(0, y, _width, lane_h - 2, _bg);
    if (i == lp.sel) { _fg = TFT_CYAN; TFT_drawRect(1, y, _width - 2, lane_h - 3, _fg); }

    char buf[12];
    _bg = LANE_BG; _fg = TFT_WHITE;
    snprintf(buf, sizeof(buf), "%d", i + 1);
    TFT_print(buf, 6, y + 6);

    // loop bar with recorded fill + playhead
    int bx = 26, bw = _width - 150, bh = 16, by = y + 6;
    _fg = (color_t){40, 60, 110};
    TFT_drawRect(bx, by, bw, bh, _fg);
    if (t->len > 0) {
        color_t c = state_col(t->state);
        int pw = bw - 2;
        _bg = (color_t){24, 34, 74};
        TFT_fillRect(bx + 1, by + 1, pw, bh - 2, _bg);
        int ph = (int)((uint64_t)t->pos * pw / (t->len ? t->len : 1));
        if (ph < 1) ph = 1;
        TFT_fillRect(bx + 1, by + 1, ph, bh - 2, c);
    }

    // state word + length
    color_t c = state_col(t->state);
    _fg = c; _bg = LANE_BG;
    TFT_print((char*)state_word(t->state), bx + bw + 8, y + 6);
    if (t->len > 0) {
        snprintf(buf, sizeof(buf), "%lus", (unsigned long)(t->len / LP_RATE));
        _fg = TFT_LIGHTGREY;
        TFT_print(buf, _width - 40, y + 6);
    }
    (void)full;
}

static void draw_header(void){
    int fh = TFT_getfontheight();
    color_t bar = {10, 18, 56};
    _bg = bar;
    TFT_fillRect(0, 0, _width, fh + 8, _bg);
    _fg = TFT_WHITE;
    TFT_print("Looper", 6, 4);
    char buf[24];
    if (lp.locked) snprintf(buf, sizeof(buf), "%.1f BPM [CLK]", lp.bpm);
    else           snprintf(buf, sizeof(buf), "-- BPM");
    _fg = lp.locked ? (color_t){40,200,90} : TFT_LIGHTGREY;
    TFT_print(buf, _width - TFT_getStringWidth(buf) - 6, 4);
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    _bg = TFT_BLACK;
    TFT_fillScreen(TFT_BLACK);
    draw_header();
    for (int i = 0; i < LP_TRACKS; i++) draw_lane(i, true);
}

static int looper_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU:
            live_full_redraw();
            break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST:
            draw_header();
            for (int i = 0; i < LP_TRACKS; i++) draw_lane(i, false);
            break;
        case EV_FWD:
            lp.sel = (lp.sel + 1) % LP_TRACKS;
            for (int i = 0; i < LP_TRACKS; i++) draw_lane(i, false);
            break;
        case EV_BWD:
            lp.sel = (lp.sel + LP_TRACKS - 1) % LP_TRACKS;
            for (int i = 0; i < LP_TRACKS; i++) draw_lane(i, false);
            break;
        case EV_SHORT_PRESS:
            // encoder press = the TR1 action on the selected lane
            // (arm/cancel/punch cycle) so the panel works without patching
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
static const char *setup_labels[] = {"Sync", "Clock Src", "Bars"};
#define SETUP_N 3

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
        char v[16];
        switch(i){
            case 0: snprintf(v, sizeof(v), "%s", lp.sync_on ? "ON" : "OFF"); break;
            case 1:
                if (lp.clk_src == LP_CLK_TR1)      snprintf(v, sizeof(v), "TR1");
                else if (lp.clk_src == LP_CLK_TR2) snprintf(v, sizeof(v), "TR2");
                else snprintf(v, sizeof(v), "CV%d", lp.clk_src + 1);
                break;
            case 2: snprintf(v, sizeof(v), "%d", lp.bars); break;
        }
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
}

static int looper_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; setup_redraw(pos, sel); break;
        case EV_FWD:
            if(sel){
                if(pos==0) lp.sync_on = !lp.sync_on;
                else if(pos==1) lp.clk_src = (lp.clk_src + 1) % LP_CLK_SRCS;
                else { int b = lp.bars * 2; lp.bars = (b > 8) ? 1 : b; }
            } else pos = (pos + 1) % SETUP_N;
            setup_redraw(pos, sel);
            break;
        case EV_BWD:
            if(sel){
                if(pos==0) lp.sync_on = !lp.sync_on;
                else if(pos==1) lp.clk_src = (lp.clk_src + LP_CLK_SRCS - 1) % LP_CLK_SRCS;
                else { int b = lp.bars / 2; lp.bars = (b < 1) ? 8 : b; }
            } else pos = (pos + SETUP_N - 1) % SETUP_N;
            setup_redraw(pos, sel);
            break;
        case EV_SHORT_PRESS: sel = !sel; setup_redraw(pos, sel); break;
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

// main screen: draw the lanes as a live backdrop and hint how to interact
static int looper_main_event(int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU:
        case EV_TIMER_REPEATING_SLOW:
            draw_header();
            for (int i = 0; i < LP_TRACKS; i++) draw_lane(i, false);
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
