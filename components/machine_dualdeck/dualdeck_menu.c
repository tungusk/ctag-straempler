// Dual-deck UI — Live (side-by-side panels + crossfade meter + big tempo),
// Setup (house row grammar), Load browser (dated, newest first) for the
// focused deck. All drawing change-driven (house discipline).
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "audio.h"
#include "sample_ram.h"
#include "dualdeck_priv.h"

static const color_t COL_PLAY = {40, 200, 90};
static const color_t COL_STOP = {70, 90, 140};
static const color_t COL_ARM  = {230, 170, 0};
static const color_t WF_GREY  = {125, 125, 135};
static const color_t PANEL_BG = {5, 9, 28};

static char (*s_samples)[24] = NULL;
static int  s_n_samples = 0, s_sample_idx = 0;

static void refresh_samples(void){
    s_n_samples = sample_list_recent(&s_samples);
    s_sample_idx = 0;
    for (int i = 0; i < s_n_samples; i++)
        if (strcmp(s_samples[i], dd.d[dd.focus].track) == 0) { s_sample_idx = i; break; }
}

// ---- Live -------------------------------------------------------------------
#define PANEL_Y  (TFT_getfontheight() + 34)
#define PANEL_H  92
#define BAR_H    30

static int  s_last_state[2] = {-1, -1};   // 0 stop, 1 play, 2 armed
static int  s_last_focus = -1;
static int  s_last_barx[2] = {-1, -1};
static int  s_last_wf[2] = {-1, -1};
static char s_last_track[2][DD_NAME_LEN];
static int  s_last_dbpm = -1;
static int  s_last_xfx = -1;

static int panel_state(int i){
    dd_deck_t *v = &dd.d[i];
    if (v->loop_active) return 3;
    if (v->arm_start || v->arm_stop) return 2;
    return v->playing ? 1 : 0;
}
static color_t state_col(int st){
    if (st == 3) return (color_t){150, 45, 95};   // loop pink (house shade)
    return st == 1 ? COL_PLAY : st == 2 ? COL_ARM : COL_STOP;
}

// display EMA of the shared external tempo (deck pattern: raw per-pulse bpm
// dances ~±0.35 from block-quantized edges; snap through real changes)
static float ext_bpm_disp(void){
    static float ema = 0;
    static uint32_t last_tk = 0;
    float x = clockin_beat_bpm(&dd.ci);
    if (x <= 0) { ema = 0; return 0; }
    float d = x - ema;
    if (ema <= 0 || d > 2.0f || d < -2.0f) { ema = x; return ema; }
    uint32_t tk = xTaskGetTickCount();
    if (tk - last_tk >= pdMS_TO_TICKS(250)) { last_tk = tk; ema += 0.08f * d; }
    return ema;
}

static void draw_big_bpm(void){
    float bpm = ext_bpm_disp();
    int d = (int)(bpm * 10.0f + 0.5f);
    if (d == s_last_dbpm) return;
    s_last_dbpm = d;
    char s[16];
    if (d > 0) snprintf(s, sizeof(s), "%d.%d", d / 10, d % 10);
    else snprintf(s, sizeof(s), "---");
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    int bw = 120, bh = TFT_getfontheight() + 4;
    _bg = TFT_BLACK;
    TFT_fillRect(_width - bw, 2, bw, bh, _bg);
    _fg = dd.ci.clk.locked ? COL_PLAY : TFT_WHITE;
    TFT_print(s, _width - TFT_getStringWidth(s) - 8, 4);
    cfont = f;
}

// bar canvas + waveform for one panel, restricted to [sx, sx+sw)
static void bar_slice(int i, int px, int pw, int sx, int sw){
    dd_deck_t *v = &dd.d[i];
    int bx = px + 3, bw = pw - 6;
    int by = PANEL_Y + PANEL_H - BAR_H - 4;
    if (sx < bx) { sw -= bx - sx; sx = bx; }
    if (sx + sw > bx + bw) sw = bx + bw - sx;
    if (sw <= 0) return;
    TFT_fillRect(sx, by, sw, BAR_H, (color_t){0, 0, 0});
    if (v->wf_state == 2){
        int wy = by + 2, wh = BAR_H - 4;
        for (int c = 0; c < bw - 1; c += 2){
            int x = bx + c;
            if (x + 2 <= sx || x >= sx + sw) continue;
            float a = sqrtf((float)v->wf[(c * DD_WF_W) / bw] / 255.0f);
            int h = (int)(a * (float)wh);
            if (h < 2) h = 2;
            TFT_fillRect(x, wy + (wh - h) / 2, 2, h, WF_GREY);
        }
    }
}

static void draw_playhead(int i, int px, int pw){
    dd_deck_t *v = &dd.d[i];
    int bx = px + 3, bw = pw - 6;
    int by = PANEL_Y + PANEL_H - BAR_H - 4;
    int ph = -1;
    if (v->file_frames && v->track[0]){
        uint32_t p = v->rpos_i < v->file_frames ? v->rpos_i : v->file_frames;
        ph = (int)((uint64_t)p * (bw - 3) / v->file_frames);
    }
    if (ph == s_last_barx[i]) return;
    if (s_last_barx[i] >= 0) bar_slice(i, px, pw, bx + s_last_barx[i], 3);
    if (ph >= 0){
        _bg = state_col(panel_state(i));
        TFT_fillRect(bx + ph, by, 3, BAR_H, _bg);
    }
    s_last_barx[i] = ph;
}

static void draw_panel(int i, bool full){
    dd_deck_t *v = &dd.d[i];
    int pw = _width / 2 - 6;
    int px = (i == 0) ? 4 : _width / 2 + 2;
    int st = panel_state(i);
    bool focus = (i == dd.focus);
    bool repaint = full || st != s_last_state[i] || (focus != (s_last_focus == i)) ||
                   strcmp(v->track, s_last_track[i]) != 0 ||
                   (v->wf_state == 2 ? 1 : 0) != s_last_wf[i];
    if (!repaint){ draw_playhead(i, px, pw); return; }
    s_last_state[i] = st;
    s_last_wf[i] = v->wf_state == 2 ? 1 : 0;
    strlcpy(s_last_track[i], v->track, DD_NAME_LEN);

    _bg = PANEL_BG;
    TFT_fillRect(px, PANEL_Y, pw, PANEL_H, _bg);
    _fg = focus ? TFT_WHITE : (color_t){40, 60, 110};
    TFT_drawRect(px, PANEL_Y, pw, PANEL_H, _fg);
    if (focus) TFT_drawRect(px + 1, PANEL_Y + 1, pw - 2, PANEL_H - 2, _fg);

    char line[32];
    _fg = TFT_WHITE;
    snprintf(line, sizeof(line), "%c %.11s", i == 0 ? 'A' : 'B',
             v->track[0] ? v->track : "(empty)");
    TFT_print(line, px + 6, PANEL_Y + 6);

    _fg = state_col(st);
    char sw[12];
    if (st == 3) snprintf(sw, sizeof(sw), "LOOP %d", v->loop_beats);
    else snprintf(sw, sizeof(sw), "%s", st == 1 ? "PLAY" : st == 2 ? "ARM" : "STOP");
    if (v->track_bpm > 0) snprintf(line, sizeof(line), "%s  %.1f", sw, v->track_bpm);
    else snprintf(line, sizeof(line), "%s  no grid", sw);
    TFT_print(line, px + 6, PANEL_Y + 6 + TFT_getfontheight() + 4);

    bar_slice(i, px, pw, px + 3, pw - 6);
    s_last_barx[i] = -1;
    draw_playhead(i, px, pw);
}

// thin crossfade meter bridging the panels
static void draw_xfade(bool full){
    int y = PANEL_Y + PANEL_H + 8;
    int mw = _width - 80, mx = 40, mh = 8;
    int xfx = (int)(dd.xf * (float)(mw - 4));
    if (!full && xfx == s_last_xfx) return;
    s_last_xfx = xfx;
    _fg = (color_t){40, 60, 110};
    TFT_drawRect(mx, y, mw, mh, _fg);
    _bg = TFT_BLACK;
    TFT_fillRect(mx + 1, y + 1, mw - 2, mh - 2, _bg);
    _bg = dd.auto_active ? COL_ARM : (color_t){60, 150, 220};
    TFT_fillRect(mx + 1 + xfx, y + 1, 3, mh - 2, _bg);
    _fg = TFT_LIGHTGREY;
    TFT_print("A", mx - 14, y - 3);
    TFT_print("B", mx + mw + 6, y - 3);
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _fg = TFT_WHITE;
    TFT_print("DualDeck", 6, 4);
    s_last_dbpm = -1;
    s_last_focus = -1;
    for (int i = 0; i < 2; i++){ s_last_state[i] = -1; s_last_track[i][0] = 0; }
    draw_big_bpm();
    for (int i = 0; i < 2; i++) draw_panel(i, true);
    s_last_focus = dd.focus;
    draw_xfade(true);
    _bg = TFT_BLACK; _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn:focus press:load hold:setup TR1:start/stop TR2:loop", 6,
              _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int dd_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU:
            live_full_redraw();
            break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST: {
            draw_big_bpm();
            for (int i = 0; i < 2; i++) draw_panel(i, false);
            s_last_focus = dd.focus;
            draw_xfade(false);
            if (event == EV_TIMER_REPEATING_SLOW){
                char dbg[64];
                snprintf(dbg, sizeof(dbg), "A%c s%lu E%+d | B%c s%lu E%+d | x%d p%lu L%d",
                         dd.d[0].loop_active ? 'L' : (dd.d[0].playing ? 'P' : 's'),
                         (unsigned long)dd.d[0].dbg_starve,
                         (int)(dd.d[0].phase_err * 100),
                         dd.d[1].loop_active ? 'L' : (dd.d[1].playing ? 'P' : 's'),
                         (unsigned long)dd.d[1].dbg_starve,
                         (int)(dd.d[1].phase_err * 100),
                         (int)(dd.xf * 100),
                         (unsigned long)(dd.ci.clk.period / 44),
                         (int)dd.ci.clk.locked);
                audio_status_set_voices("dualdeck", dbg);
            }
            break;
        }
        case EV_FWD:
        case EV_BWD:
            dd.focus = 1 - dd.focus;
            for (int i = 0; i < 2; i++) draw_panel(i, false);
            s_last_focus = dd.focus;
            break;
        case EV_SHORT_PRESS:
            refresh_samples();
            return M_DD_LOAD;
        case EV_LONG_PRESS:
            return M_DD_SETUP;
        default: break;
    }
    return 0;
}

// ---- Load browser (deck/sampler3 style: big centered name, dated order) --------
static void load_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_LIGHTGREY;
    char h[48];
    snprintf(h, sizeof(h), "Load -> deck %c  (%d/%d)", dd.focus == 0 ? 'A' : 'B',
             s_n_samples ? s_sample_idx + 1 : 0, s_n_samples);
    TFT_print(h, _width / 2 - TFT_getStringWidth(h) / 2, 4);
    if (!s_n_samples){
        char *m = "no files in usr/";
        TFT_print(m, _width / 2 - TFT_getStringWidth(m) / 2, _height / 2);
        return;
    }
    int cy = _height / 2;
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    int bigfh = TFT_getfontheight();
    _fg = TFT_WHITE;
    char *selnm = s_samples[s_sample_idx];
    TFT_print(selnm, _width / 2 - TFT_getStringWidth(selnm) / 2, cy - bigfh / 2);
    cfont = f;
    _fg = (color_t){110, 110, 110};
    if (s_sample_idx > 0){
        char *p = s_samples[s_sample_idx - 1];
        TFT_print(p, _width / 2 - TFT_getStringWidth(p) / 2, cy - bigfh / 2 - TFT_getfontheight() - 10);
    }
    if (s_sample_idx < s_n_samples - 1){
        char *p = s_samples[s_sample_idx + 1];
        TFT_print(p, _width / 2 - TFT_getStringWidth(p) / 2, cy + bigfh / 2 + 10);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("press:load  hold:back", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int dd_load_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: load_redraw(); break;
        case EV_FWD: if (s_sample_idx < s_n_samples - 1){ s_sample_idx++; load_redraw(); } break;
        case EV_BWD: if (s_sample_idx > 0){ s_sample_idx--; load_redraw(); } break;
        case EV_SHORT_PRESS:
            if (s_n_samples) dualdeck_load_track(dd.focus, s_samples[s_sample_idx]);
            return M_DD_LIVE;
        case EV_LONG_PRESS: return M_DD_LIVE;
        default: break;
    }
    return 0;
}

// ---- Setup (house grammar: toggles click, lists bracket-edit, per-row paint) ----
static const char *setup_labels[] = {"Clock Src", "Clock", "Fade", "Loop Len"};
#define DD_SETUP_N 4
#define SETUP_IS_TOGGLE(i) ((i) == 2 || (i) == 3)   // short cycles: click advances
#define SETUP_ROW_Y(i) (TFT_getfontheight() + 12 + (i) * (TFT_getfontheight() + 8))

static void setup_value_str(int i, char *v, size_t n){
    switch(i){
        case 0: snprintf(v, n, "CV%d", dd.clk_src + 1); break;
        case 1: snprintf(v, n, "%s", dd_ppb_names[dd.ppb_idx]); break;
        case 2:
            if (dd.fade_beats == 0) snprintf(v, n, "cut");
            else snprintf(v, n, "%d beats", dd.fade_beats);
            break;
        case 3: snprintf(v, n, "%d beats", dd.loop_len_beats); break;
        default: v[0] = 0;
    }
}

static void setup_adj(int i, int dir){
    switch(i){
        case 0: dd.clk_src = (dd.clk_src + (dir > 0 ? 1 : 7)) & 7; break;
        case 1:
            dd.ppb_idx += dir;
            if (dd.ppb_idx < 0) dd.ppb_idx = 0;
            if (dd.ppb_idx > 5) dd.ppb_idx = 5;
            break;
        case 2: {
            static const int steps[4] = {0, 1, 4, 8};
            int k = 0;
            for (int s = 0; s < 4; s++) if (steps[s] == dd.fade_beats) k = s;
            k = (k + (dir > 0 ? 1 : 3)) % 4;
            dd.fade_beats = steps[k];
            break;
        }
        case 3: {
            static const int lsteps[5] = {1, 2, 4, 8, 16};
            int k = 2;
            for (int s = 0; s < 5; s++) if (lsteps[s] == dd.loop_len_beats) k = s;
            k = (k + (dir > 0 ? 1 : 4)) % 5;
            dd.loop_len_beats = lsteps[k];   // next engage uses it
            break;
        }
    }
}

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
    if (editing) snprintf(val, sizeof(val), "[ %s ]", raw);
    else snprintf(val, sizeof(val), "%s", raw);
    TFT_print(val, _width - TFT_getStringWidth(val) - 10, y);
    _bg = TFT_BLACK;
}

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    _fg = TFT_WHITE;
    TFT_print("DualDeck Setup", 6, 4);
    menuTFTPrintAffordance("Machine", pos == -1);
    for (int i = 0; i < DD_SETUP_N; i++) setup_row_redraw(i, pos, sel);
}

static int dd_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; setup_redraw(pos, sel); break;
        case EV_FWD:
        case EV_BWD: {
            int dir = (event == EV_FWD) ? +1 : -1;
            if(sel){
                setup_adj(pos, dir);
                setup_row_redraw(pos, pos, sel);
            } else {
                pos += dir;
                if(pos >= DD_SETUP_N) pos = -1;
                if(pos < -1) pos = DD_SETUP_N - 1;
                setup_redraw(pos, sel);
            }
            break;
        }
        case EV_SHORT_PRESS:
            if(pos == -1) return M_MORE;
            if(SETUP_IS_TOGGLE(pos)){
                setup_adj(pos, +1);
                setup_row_redraw(pos, pos, 0);
            } else {
                sel = !sel;
                setup_row_redraw(pos, pos, sel);
            }
            break;
        case EV_LONG_PRESS: return M_DD_LIVE;
        default: break;
    }
    return 0;
}

// ---- registration ---------------------------------------------------------
static void dd_register_pages(void *menusys){
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_DD_LIVE);
    menusys_item_set_default_cb(_ms, M_DD_LIVE, dd_live_handler);
    menusys_new_item(_ms, M_DD_SETUP);
    menusys_item_set_default_cb(_ms, M_DD_SETUP, dd_setup_handler);
    menusys_new_item(_ms, M_DD_LOAD);
    menusys_item_set_default_cb(_ms, M_DD_LOAD, dd_load_handler);
}

static const char *const dd_main_items[] = {"Live", "Setup"};
static const int dd_main_targets[] = {M_DD_LIVE, M_DD_SETUP};

const machine_ui_t dualdeck_menu_ui = {
    .main_items = dd_main_items,
    .main_targets = dd_main_targets,
    .n_main = 2,
    .register_pages = dd_register_pages,
    .boot_target = M_DD_LIVE,
};
