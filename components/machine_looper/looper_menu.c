// M2 looper UI — a Live lane view (performance) and a Setup page.
// House style (sampler3/drums era, 2026-07-13 refresh): lanes carry a real
// waveform thumbnail on a black canvas with the state-colored playhead as the
// only bright thing in the bar; Setup toggles flip on click, list/range rows
// keep click-to-edit with an explicit "[ value ]" bracket; value edits repaint
// ONE row, playhead moves repaint slices — never the whole lane.
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include <esp_http_server.h>
#include "machine.h"
#include "clock.h"
#include "beatlisten.h"
#include "menu_config.h"
#include "looper_priv.h"

// Deck's transport grammar, ported to the 4-lane view (Arlo): the state lives
// on a FAT color-coded box around the bar, the playhead is a neutral WHITE line
// (not state-colored), and the selected lane wears its number as a WHITE PLATE
// (the DoubleDecker focus cue). The grey waveform on a black canvas is unchanged.
static const color_t LANE_BG   = {5, 9, 28};      // darker blue track background
static const color_t BAR_BG    = {14, 22, 52};    // empty-lane bar region
static const color_t COL_EMPTY = {45, 60, 95};    // dim: the box barely reads
static const color_t COL_ARMED = {230, 170, 0};
static const color_t COL_REC   = {220, 40, 40};
static const color_t COL_PLAY  = {40, 200, 90};
static const color_t COL_STOP  = {45, 90, 170};   // deck's stopped-blue family
static const color_t WF_GREY   = {125, 125, 135}; // waveform: reads under the white playhead
static const color_t PH_COL    = {240, 240, 245}; // playhead: transport-neutral white (deck)

static const char *state_word(int s){
    switch(s){ case LP_ARMED: return "ARM"; case LP_REC: return "REC";
               case LP_PLAY: return "PLAY"; case LP_STOP: return "STOP";
               default: return "---"; }
}
// the STATE colour now drives the fat box border (deck's tbar_bg role)
static color_t state_col(int s){
    switch(s){ case LP_ARMED: return COL_ARMED; case LP_REC: return COL_REC;
               case LP_PLAY: return COL_PLAY;   case LP_STOP: return COL_STOP;
               default: return COL_EMPTY; }
}

// ---- Live lane view -------------------------------------------------------
// Incremental drawing: full chrome only on state/selection change, playhead
// moves restore a slice of the canvas + waveform and stamp the new line.
static uint8_t s_last_state[LP_TRACKS];
static int     s_last_ph[LP_TRACKS];     // playhead x within the bar (-1 = none)
static int     s_last_vol[LP_TRACKS];
static int     s_last_sel = -1;
static bool    s_last_locked;
static int     s_last_bpm10 = -12345;
static void    lane_vol(int i);          // defined after lane_chrome

// waveform thumbnails, built from the PSRAM loop buffers (UI task; ~10k
// reads per build — instant). s_wf_for is the length a build was last
// ATTEMPTED for (even if it produced nothing), so a too-short loop doesn't
// retrigger the builder every timer tick.
#define WF_W 160
static uint8_t  s_wf[LP_TRACKS][WF_W];
static uint32_t s_wf_for[LP_TRACKS];
static bool     s_wf_ok[LP_TRACKS];

static void lanes_reset_cache(void){
    for (int i = 0; i < LP_TRACKS; i++){
        s_last_state[i] = 0xFF; s_last_ph[i] = -1; s_last_vol[i] = -1;
        s_wf_for[i] = 0; s_wf_ok[i] = false;
    }
    s_last_sel = -1; s_last_bpm10 = -12345; s_last_locked = false;
}

static int lane_y(int i){ return TFT_getfontheight() + 11 + i * 46; }
#define LANE_BX 26
#define LANE_BW (_width - 150)
#define LANE_BH 26
#define BOX_BW 2          // fat state-box border (deck's TBAR_BW, scaled to the lane)

// the fat state box around a lane's bar — deck's "state on the border, not the
// fill" idiom. Four segments so the interior (canvas + waveform + playhead) is
// never touched. The bar region is (LANE_BX, y+6) .. +(LANE_BW, LANE_BH).
static void lane_box(int y, color_t c){
    _bg = c;
    TFT_fillRect(LANE_BX, y + 6, LANE_BW, BOX_BW, _bg);                       // top
    TFT_fillRect(LANE_BX, y + 6 + LANE_BH - BOX_BW, LANE_BW, BOX_BW, _bg);    // bottom
    TFT_fillRect(LANE_BX, y + 6, BOX_BW, LANE_BH, _bg);                       // left
    TFT_fillRect(LANE_BX + LANE_BW - BOX_BW, y + 6, BOX_BW, LANE_BH, _bg);    // right
}

static void wf_build(int i){
    lp_track_t *t = &lp.tr[i];
    uint32_t len = t->len;
    s_wf_for[i] = len;
    s_wf_ok[i] = false;
    if (!t->buf || len < WF_W) return;
    for (int c = 0; c < WF_W; c++){
        uint32_t a = (uint32_t)((uint64_t)c * len / WF_W);
        uint32_t b = (uint32_t)((uint64_t)(c + 1) * len / WF_W);
        uint32_t step = (b - a) / 48 + 1;   // cap ~48 taps per column
        int peak = 0;
        for (uint32_t s = a; s < b; s += step){
            int v = t->buf[s]; if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        peak >>= 7;                          // 0..32767 -> 0..255
        s_wf[i][c] = (uint8_t)(peak > 255 ? 255 : peak);
    }
    s_wf_ok[i] = true;
}

// during REC the loop length isn't known yet, so show progress toward the
// auto-stop target (bar-quantized length); otherwise show play position
static uint32_t lane_denom(lp_track_t *t){
    return (t->state == LP_REC) ? t->target : t->len;
}

// bar canvas + waveform, restricted to the [sx, sx+sw) slice — the playhead
// restore path repaints 4px of this instead of strobing the whole bar
static void lane_bar_slice(int i, int sx, int sw){
    int y = lane_y(i);
    lp_track_t *t = &lp.tr[i];
    int bx = LANE_BX + BOX_BW, by = y + 6 + BOX_BW, bh = LANE_BH - 2*BOX_BW, bw = LANE_BW - 2*BOX_BW;
    if (sx < bx) { sw -= bx - sx; sx = bx; }
    if (sx + sw > bx + bw) sw = bx + bw - sx;
    if (sw <= 0) return;
    bool loaded = (t->len > 0) || (t->state == LP_REC);
    TFT_fillRect(sx, by, sw, bh, loaded ? (color_t){0, 0, 0} : BAR_BG);
    // BOLD 2px strokes, medium grey, sqrt amplitude lift (the sampler3
    // playbar treatment — white-on-white swallowed the playhead there)
    if (s_wf_ok[i] && s_wf_for[i] == t->len){
        int wy = by + 2, wh = bh - 4;
        for (int c = 0; c < bw - 1; c += 2){
            int x = bx + c;
            if (x + 2 <= sx || x >= sx + sw) continue;
            float a = sqrtf((float)s_wf[i][(c * WF_W) / bw] / 255.0f);
            int h = (int)(a * (float)wh);
            if (h < 2) h = 2;
            TFT_fillRect(x, wy + (wh - h) / 2, 2, h, WF_GREY);
        }
    }
}

// stamp the playhead (fat state-colored line); restores the old position's
// slice first. Full-bar callers pass restore=false after a fresh canvas.
#define PH_W 3
static void lane_playhead(int i, bool restore){
    int y = lane_y(i);
    lp_track_t *t = &lp.tr[i];
    int bx = LANE_BX + BOX_BW, by = y + 6 + BOX_BW, bh = LANE_BH - 2*BOX_BW, bw = LANE_BW - 2*BOX_BW;
    uint32_t denom = lane_denom(t);
    int ph = -1;
    if (denom > 0){
        ph = (int)((uint64_t)t->pos * bw / denom);
        if (ph > bw - PH_W) ph = bw - PH_W;
        if (ph < 0) ph = 0;
    }
    if (restore && ph == s_last_ph[i]) return;   // parked: nothing to repaint
    if (restore && s_last_ph[i] >= 0)
        lane_bar_slice(i, bx + s_last_ph[i], PH_W);
    if (ph >= 0){
        _bg = PH_COL;                    // white, transport-neutral (deck)
        TFT_fillRect(bx + ph, by, PH_W, bh, _bg);
    }
    s_last_ph[i] = ph;
}

// full lane chrome: number plate, fat state box, bar, state word, length
static void lane_chrome(int i){
    int y = lane_y(i);
    int fh = TFT_getfontheight();
    lp_track_t *t = &lp.tr[i];
    _bg = LANE_BG;
    TFT_fillRect(0, y, _width, 44, _bg);

    // lane number: a WHITE PLATE when this lane is selected (the DoubleDecker
    // focus cue — the selection must be unmissable), a dim number otherwise.
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", i + 1);
    if (i == lp.sel){
        _bg = TFT_WHITE;
        TFT_fillRect(3, y + 6, 13, fh + 3, _bg);
        _fg = (color_t){0, 0, 0};                 // black number on the white plate
        TFT_print(buf, 6, y + 7);
    } else {
        _fg = (color_t){110, 120, 150}; _bg = LANE_BG;
        TFT_print(buf, 6, y + 7);
    }

    // canvas + waveform, then the fat state box around it (state on the border,
    // not the fill — deck). The box replaces the old thin outline AND the
    // full-lane selection frame.
    lane_bar_slice(i, LANE_BX + BOX_BW, LANE_BW - 2*BOX_BW);
    lane_box(y, state_col(t->state));
    s_last_ph[i] = -1;
    lane_playhead(i, false);

    _fg = state_col(t->state); _bg = LANE_BG;
    TFT_print((char*)state_word(t->state), LANE_BX + LANE_BW + 8, y + 6);
    if (t->len > 0){
        snprintf(buf, sizeof(buf), "%lus", (unsigned long)(t->len / LP_RATE));
        _fg = TFT_LIGHTGREY;
        TFT_print(buf, _width - 58, y + 6);
    }
    lane_vol(i);
}

// per-track volume: a small vertical level bar at the lane's right edge,
// reflecting t->vol (CV6 on the selected track). Cached in pixels so it only
// repaints on a visible change.
#define VOL_X (_width - 9)
#define VOL_H 30
static void lane_vol(int i){
    int y = lane_y(i) + 7;
    lp_track_t *t = &lp.tr[i];
    _fg = (color_t){40, 60, 110};
    TFT_drawRect(VOL_X, y, 6, VOL_H, _fg);
    int fill = (int)((uint32_t)t->vol * (VOL_H - 2) / 255);
    _bg = LANE_BG;
    TFT_fillRect(VOL_X + 1, y + 1, 4, VOL_H - 2, _bg);
    if (fill > 0){
        _bg = (color_t){60, 150, 220};
        TFT_fillRect(VOL_X + 1, y + 1 + (VOL_H - 2 - fill), 4, fill, _bg);
    }
    s_last_vol[i] = t->vol >> 3;   // ~1 cache step per pixel
}

static void lanes_update(void){
    for (int i = 0; i < LP_TRACKS; i++){
        lp_track_t *t = &lp.tr[i];
        // thumbnail freshness: rebuild when a stable loop's length changed
        // (record finished / cleared). Never build mid-REC — the buffer is
        // still being written and the length isn't final.
        uint32_t want = (t->state == LP_PLAY || t->state == LP_STOP) ? t->len : 0;
        bool wf_stale = (want != s_wf_for[i]);
        if (wf_stale && want) wf_build(i);
        else if (wf_stale) { s_wf_for[i] = 0; s_wf_ok[i] = false; }

        bool chrome = (t->state != s_last_state[i]) ||
                      (i == lp.sel) != (i == s_last_sel) || wf_stale;
        if (chrome){ lane_chrome(i); s_last_state[i] = t->state; }
        else {
            lane_playhead(i, true);
            if ((t->vol >> 3) != s_last_vol[i]) lane_vol(i);
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
    // control hint along the bottom — the full grammar, house format
    _bg = TFT_BLACK; _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn:sel press/TR1:rec TR2:play/stop hold:setup", 6,
              _height - TFT_getfontheight() - 1);
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
            return M_LOOPER_SETUP;   // toggle Live -> Setup (no hub)
        default:
            break;
    }
    return 0;
}

// ---- Setup page -----------------------------------------------------------
// House row behavior (piloted on sampler3, Arlo 2026-07-12): simple TOGGLES
// flip right on the click — no edit mode; lists/ranges keep click-to-edit +
// turn with an explicit "[ value ]" bracket while editing. Value edits and
// toggles repaint ONE row; only navigation repaints the page.
static const char *setup_labels[] = {"Sync", "Clock Src", "Bars", "Monitor",
                                     "BP Filter", "Clock PPQ", "Save Trk", "Bounce"};
#define SETUP_N 8
#define SETUP_PPQ_ROW 5
#define SETUP_SAVE_ROW 6
#define SETUP_BOUNCE_ROW 7
#define SETUP_IS_TOGGLE(i) ((i) == 0 || (i) == 3 || (i) == 4)
#define SETUP_ROW_Y(i) (TFT_getfontheight() + 12 + (i) * (TFT_getfontheight() + 8))
static const char *s_save_msg = "";   // transient result shown on the Save row
static const char *s_bounce_msg = ""; // transient result shown on the Bounce row

static void setup_value_str(int i, char *v, size_t n){
    switch(i){
        case 0: snprintf(v, n, "%s", lp.sync_on ? "ON" : "OFF"); break;
        case 1: snprintf(v, n, "%s", clock_source_name(lp.clk_src)); break;
        case 2: snprintf(v, n, "%d", lp.bars); break;
        case 3: snprintf(v, n, "%s", lp.monitor ? "ON" : "OFF"); break;
        case 4: snprintf(v, n, "%s", lp.filter_on ? "ON" : "OFF"); break;
        case 5: snprintf(v, n, "%d", looper_get_ppq()); break;
        case 6: snprintf(v, n, "%s trk %d",
                         s_save_msg[0] ? s_save_msg : "press:", lp.sel + 1); break;
        case 7: snprintf(v, n, "%s",
                         s_bounce_msg[0] ? s_bounce_msg : "press: all->1"); break;
        default: v[0] = 0;
    }
}

static void setup_adj(int i, int dir){
    switch(i){
        case 0: lp.sync_on = !lp.sync_on; break;
        case 1:
            // full set incl. TR (the looper masks its clock trig) and AUDIO;
            // picking AUDIO switches the listener on if it was off
            lp.clk_src = (lp.clk_src + (dir > 0 ? 1 : CLK_SRC_COUNT - 1)) % CLK_SRC_COUNT;
            if (lp.clk_src == CLK_SRC_AUDIO && beatlisten_get_mode() == BL_OFF) {
                beatlisten_set_mode(BL_GROOVE);
                configSetIntSetting("blisten", BL_GROOVE);
            }
            break;
        case 2: {
            int b = (dir > 0) ? lp.bars * 2 : lp.bars / 2;
            lp.bars = (b > 8) ? 1 : (b < 1) ? 8 : b;
            break;
        }
        case 3: lp.monitor = !lp.monitor; break;
        case 5: {   // Clock PPQ ladder — Arlo's jig clocks at 8
            static const int lad[] = {1, 2, 4, 8};
            int k = 0, cur = looper_get_ppq();
            for (int q = 0; q < 4; q++) if (lad[q] == cur) k = q;
            looper_set_ppq((float)lad[(k + (dir > 0 ? 1 : 3)) % 4]);
            break;
        }
        case 4: lp.filter_on = !lp.filter_on; break;
    }
}

// one row only — value adjustments must not repaint the whole menu
static void setup_row_redraw(int i, int pos, int sel){
    int fh = TFT_getfontheight();
    int y = SETUP_ROW_Y(i);
    bool editing = (i == pos && sel);
    _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
    _fg = editing ? TFT_CYAN : TFT_WHITE;
    TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
    TFT_print((char*)setup_labels[i], 8, y);
    char raw[24], val[28];
    setup_value_str(i, raw, sizeof(raw));
    if (editing) snprintf(val, sizeof(val), "[ %s ]", raw);   // edit-mode bracket
    else snprintf(val, sizeof(val), "%s", raw);
    TFT_print(val, _width - TFT_getStringWidth(val) - 10, y);
    _bg = TFT_BLACK;
}

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    _fg = TFT_WHITE;
    TFT_print("Looper Setup", 6, 4);
    menuTFTPrintAffordance("Machine", pos == -1);
    for (int i = 0; i < SETUP_N; i++) setup_row_redraw(i, pos, sel);
}

static int looper_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU:
            pos = 0; sel = 0; s_save_msg = ""; s_bounce_msg = "";
            setup_redraw(pos, sel);
            break;
        case EV_FWD:
        case EV_BWD: {
            int dir = (event == EV_FWD) ? +1 : -1;
            if(sel){
                setup_adj(pos, dir);
                setup_row_redraw(pos, pos, sel);       // value edit: one row only
            } else {
                pos += dir;
                if(pos >= SETUP_N) pos = -1;
                if(pos < -1) pos = SETUP_N - 1;
                s_save_msg = ""; s_bounce_msg = "";
                setup_redraw(pos, sel);
            }
            break;
        }
        case EV_SHORT_PRESS:
            if(pos == -1) return M_MORE;   // System affordance
            if(pos == SETUP_SAVE_ROW){
                // action row: write the selected track to the SD library
                int r = looper_save_track(lp.sel);
                s_save_msg = (r == 0) ? "SAVED" : "EMPTY";
                setup_row_redraw(pos, pos, 0);
            } else if(pos == SETUP_BOUNCE_ROW){
                // action row: resample all tracks down into track 1
                int r = looper_bounce();
                s_bounce_msg = (r == 0) ? "BOUNCED" : (r == -2) ? "STOP REC" : "EMPTY";
                if (r == 0) lp.sel = 0;   // the bounce lands on track 1 — focus it
                setup_row_redraw(pos, pos, 0);
            } else if(SETUP_IS_TOGGLE(pos)){
                // toggles flip right here — no edit mode to enter
                setup_adj(pos, +1);
                setup_row_redraw(pos, pos, 0);
            } else {
                sel = !sel;
                setup_row_redraw(pos, pos, sel);
            }
            break;
        case EV_LONG_PRESS: return M_LOOPER_LIVE;   // toggle Setup -> Live
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

extern const httpd_uri_t looper_web_uris[];

const machine_ui_t looper_menu_ui = {
    .main_items = looper_main_items,
    .main_targets = looper_main_targets,
    .n_main = 2,
    .register_pages = looper_register_pages,
    .main_event = looper_main_event,
    .boot_target = M_LOOPER_LIVE,
    .web_uris = looper_web_uris,
    .n_web_uris = 1,
};
