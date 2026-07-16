// Sampler3 UI — Live (two voice lanes + explicit ARM/REC banner; turn =
// select voice, press = load browser, long = Setup), Setup (per-voice
// mode/reverse/pitch-source/level/pan/trim + Record page entry), Load
// (usr/*.RAW browser -> selected voice), Record (explicit arm target +
// monitor; the silent short-press arm-cycle of the old sampler is gone —
// that hidden state read as "sampler only plays monitor audio").
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "recording.h"
#include "beatlisten.h"
#include "menu_config.h"
#include "setup_menu.h"
#include "sample_browser.h"
#include "sampler3_priv.h"

static const color_t ACCENT   = {230, 160, 40};
static const color_t COL_REC  = {220, 40, 40};
static const color_t COL_ARM  = {230, 200, 40};
static const color_t COL_PLAY = {25, 120, 50};
static const color_t COL_IDLE = {30, 60, 140};

static int s_voice_sel = 0;                    // lane focus on Live / Setup target
static int s_load_ret = M_S3_LIVE;

// ---- Live ---------------------------------------------------------------------
// change-driven redraws only (full-region repaints strobe — deck lesson)

// side-by-side voice PANELS mirroring the physical layout (Arlo): left =
// track 1, right = track 2. The panel BACKGROUND is the state — blue idle,
// green playing, yellow armed, red recording; the focused panel gets a
// white border; a light marker sweeps the panel floor for position.
#define PANEL_Y     30                    // clears the corner tempo (+2px pad)
#define PANEL_H     166                   // fill the vertical space
#define PANEL_BAR_H 24                    // thick: it carries the state color
#define BANNER_Y    202
static const color_t PANEL_BG = {14, 14, 20};   // calm; only the bar is loud

static int s_lane_barx[S3_NVOICES] = {-1, -1};
static char s_lane_line[S3_NVOICES][48] = {{0}, {0}};
static int s_lane_state[S3_NVOICES] = {-1, -1};
static int s_lane_sel[S3_NVOICES] = {-1, -1};
static int s_lane_wf[S3_NVOICES] = {-1, -1};
static int s_lane_native[S3_NVOICES] = {-1, -1};
// drawn crop window lives on a 1% grid with hysteresis: a cell switches
// only when the value crosses 0.7 into the neighbor. A plain deadband
// RATCHETED on noise extremes (every redraw re-centered the reference at
// a peak, so the shading could always twitch again — "still jumpy").
static int s_lane_ccs[S3_NVOICES] = {-1, -1};     // cells, 0..100
static int s_lane_cce[S3_NVOICES] = {-1, -1};

static int crop_cell(float frac, int cur){
    float c = frac * 100.0f;
    float d = c - (float)cur;
    if (cur < 0 || d > 0.7f || d < -0.7f) return (int)(c + 0.5f);
    return cur;
}

static int s_banner_state = -1;
static int s_last_sel = -1;
static int s_last_dbpm = -1;

// external clock tempo, BIG in the top-right corner (green = locked; blank
// when no clock). EMA-smoothed: clock edges are block-quantized, so the raw
// per-pulse figure dances a few tenths (deck lesson); snaps on real changes.
static void draw_clock_bpm(void){
    // external clock (green) wins; internal clock (grey) otherwise
    float ebpm = clockin_beat_bpm(&s3.ci);
    bool ext = ebpm > 0;
    float bpm = ext ? ebpm : (s3.int_bpm > 0.5f ? s3.int_bpm : 0);
    static float ema = 0;
    static uint32_t last_tk = 0;
    if (bpm <= 0) ema = 0;
    else if (ema <= 0 || bpm - ema > 2.0f || ema - bpm > 2.0f) ema = bpm;
    else {
        // deck lesson: this draw runs from BOTH timer rates — advance the
        // EMA at most 4x/s or the effective alpha multiplies (jittery digit)
        uint32_t tk = xTaskGetTickCount();
        if (tk - last_tk >= pdMS_TO_TICKS(250)) {
            last_tk = tk;
            ema += 0.08f * (bpm - ema);
        }
    }
    int d = ema > 0 ? (int)(ema * 10.0f + 0.5f) : 0;
    int key = ext ? d : -d;                 // color change forces a repaint too
    if (key == s_last_dbpm) return;
    s_last_dbpm = key;
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    int bw = 110, bh = TFT_getfontheight() + 4;
    _bg = TFT_BLACK;
    TFT_fillRect(_width - bw, 0, bw, bh, _bg);
    if (d > 0){
        char s[16];
        snprintf(s, sizeof(s), "%d.%d", d / 10, d % 10);
        _fg = ext ? (color_t){40, 200, 90} : (color_t){150, 150, 160};
        TFT_print(s, _width - TFT_getStringWidth(s) - 8, 2);
    }
    cfont = f;
}

static int lane_state(int i){
    if (recording_is_active() && recording_get_target_vid() == i) return 3;  // recording
    if (recording_get_trig_func(i) == TRIG_FUNC_RECORD) return 2;            // armed
    return s3.v[i].playing ? 1 : 0;
}

static color_t lane_bg(int st){
    switch (st){
        case 3:  return COL_REC;
        case 2:  return (color_t){130, 110, 20};   // armed: dark yellow
        case 1:  return COL_PLAY;
        default: return COL_IDLE;
    }
}

// the playbar, deck treatment (Arlo): BLACK canvas, tiny-white waveform,
// FAT state-colored border. The CROP window lives in the waveform
// brightness — bright columns inside the loop, dimmed outside.
#define PB_BW 3     // fat state border
static const color_t WF_GREY = {125, 125, 135};   // waveform: reads under the white playhead

static void playbar_slice(int i, int px, int pw, int st, const s3_voice_t *v,
                          int sx, int sw){
    int by = PANEL_Y + PANEL_H - 2 * PANEL_BAR_H - 4;
    int bx = px + 3, bw = pw - 6;
    TFT_fillRect(sx, by, sw, PANEL_BAR_H, (color_t){0, 0, 0});
    // BOLD waveform: 2px strokes, MEDIUM GREY (Arlo) — a white waveform
    // swallowed the white playhead whole once the bar filled up; the grey
    // keeps the shape legible on black and leaves the playhead the only
    // near-white thing in the bar. sqrt amplitude lift so quiet material
    // still reads at this size.
    if (v->wf_valid){
        int wy = by + 2, wh = PANEL_BAR_H - 4;
        color_t wc = WF_GREY;
        for (int c = 0; c < bw - 1; c += 2){
            int x = bx + c;
            if (x + 2 <= sx || x >= sx + sw) continue;
            float a = sqrtf((float)v->wf[(c * S3_WF_W) / bw] / 255.0f);
            int h = (int)(a * (float)wh);
            if (h < 2) h = 2;
            TFT_fillRect(x, wy + (wh - h) / 2, 2, h, wc);
        }
    }
    // the LOOP BRACKET: the fat state-colored box ends sit AT the crop
    // points — the box IS the window (the bright/dim split didn't read)
    {
        color_t on = lane_bg(st);
        int x0 = bx + s_lane_ccs[i] * bw / 100;
        int x1 = bx + s_lane_cce[i] * bw / 100;
        if (x1 > x0 + 2 * PB_BW){
            struct { int x, w, y, h; } seg[4] = {
                { x0, x1 - x0, by, PB_BW },                        // top
                { x0, x1 - x0, by + PANEL_BAR_H - PB_BW, PB_BW },  // bottom
                { x0, PB_BW, by, PANEL_BAR_H },                    // left end
                { x1 - PB_BW, PB_BW, by, PANEL_BAR_H },            // right end
            };
            for (int k = 0; k < 4; k++){
                int a = seg[k].x > sx ? seg[k].x : sx;
                int b = (seg[k].x + seg[k].w) < (sx + sw) ? (seg[k].x + seg[k].w)
                                                          : (sx + sw);
                if (b > a) TFT_fillRect(a, seg[k].y, b - a, seg[k].h, on);
            }
        }
    }
}

static void draw_playbar(int i, int px, int pw, int st, s3_voice_t *v){
    int bx = px + 3, bw = pw - 6;
    s_lane_ccs[i] = crop_cell(v->ui_cs, s_lane_ccs[i]);   // draw FROM the cells
    s_lane_cce[i] = crop_cell(v->ui_ce, s_lane_cce[i]);
    playbar_slice(i, px, pw, st, v, bx, bw);   // black + wf + loop bracket
    s_lane_barx[i] = -1;
}

static void draw_lane(int i, bool full){
    s3_voice_t *v = &s3.v[i];
    int pw = _width / 2 - 6;                 // panel width
    int px = (i == 0) ? 4 : _width / 2 + 2;  // left / right panel
    int st = lane_state(i);
    bool sel = (i == s_voice_sel);
    char line[48];
    snprintf(line, sizeof(line), "%.11s|%s%s", v->name[0] ? v->name : "-",
             v->playmode == S3_MODE_LOOP ? "LP" : "1S", v->reverse ? "R" : "");

    bool repaint = full || st != s_lane_state[i] || sel != (s_lane_sel[i] == 1) ||
                   strcmp(line, s_lane_line[i]) != 0 ||
                   (v->wf_valid ? 1 : 0) != s_lane_wf[i];
    if (repaint){
        s_lane_state[i] = st;
        s_lane_sel[i] = sel ? 1 : 0;
        s_lane_wf[i] = v->wf_valid ? 1 : 0;
        strcpy(s_lane_line[i], line);
        TFT_fillRect(px, PANEL_Y, pw, PANEL_H, PANEL_BG);
        _fg = sel ? TFT_WHITE : (color_t){70, 70, 90};
        TFT_drawRect(px, PANEL_Y, pw, PANEL_H, _fg);
        if (sel) TFT_drawRect(px + 1, PANEL_Y + 1, pw - 2, PANEL_H - 2, _fg);
        // big track numeral + text stack: track 2 mirrors track 1 (Arlo) —
        // its numeral/name/mode/state hug the RIGHT edge
        bool mirror = (i == 1);
        Font f = cfont;
        TFT_setFont(DEJAVU24_FONT, NULL);
        char num[12];
        snprintf(num, sizeof(num), "%d", i + 1);
        _bg = PANEL_BG;
        _fg = TFT_WHITE;
        TFT_print(num, mirror ? px + pw - TFT_getStringWidth(num) - 8 : px + 8,
                  PANEL_Y + 8);
        cfont = f;
        // name + mode, wrapped small under the numeral
        _fg = sel ? TFT_WHITE : TFT_LIGHTGREY;
        char nm[16], md[12];
        snprintf(nm, sizeof(nm), "%.12s", v->name[0] ? v->name : "(empty)");
        snprintf(md, sizeof(md), "%s%s", v->playmode == S3_MODE_LOOP ? "LOOP" : "1SHOT",
                 v->reverse ? " REV" : "");
        TFT_print(nm, mirror ? px + pw - TFT_getStringWidth(nm) - 8 : px + 8,
                  PANEL_Y + 44);
        TFT_print(md, mirror ? px + pw - TFT_getStringWidth(md) - 8 : px + 8,
                  PANEL_Y + 64);
        const char *stn = st == 3 ? "REC" : st == 2 ? "ARMED" : st == 1 ? "PLAY" : "";
        if (stn[0]) TFT_print((char*)stn,
                              mirror ? px + pw - TFT_getStringWidth((char*)stn) - 8 : px + 8,
                              PANEL_Y + 84);
        // total length of the loaded audio, right-justified under the bar
        if (v->play_len){
            char tl[16];
            unsigned long ts = v->play_len / S3_RATE;
            unsigned long td = (unsigned long)((uint64_t)(v->play_len % S3_RATE) * 10 / S3_RATE);
            if (ts < 60) snprintf(tl, sizeof(tl), "%lu.%lus", ts, td);
            else snprintf(tl, sizeof(tl), "%lu:%02lu", ts / 60, ts % 60);
            _fg = TFT_LIGHTGREY;
            _bg = PANEL_BG;
            TFT_print(tl, px + pw - TFT_getStringWidth(tl) - 6,
                      PANEL_Y + PANEL_H - PANEL_BAR_H);
        }
        // the waveform + crop + state all live in the playbar now
        draw_playbar(i, px, pw, st, v);
        _bg = TFT_BLACK;
        s_lane_native[i] = -1;      // badge redraws after a full repaint
    }
    // crop moved (knob/CV performance): re-shade the BAR only — the waveform
    // thumbnail is never rebuilt for crop (Arlo). Repaint only when the
    // HYSTERETIC cells actually move — noise can't twitch them.
    {
        if (crop_cell(v->ui_cs, s_lane_ccs[i]) != s_lane_ccs[i] ||
            crop_cell(v->ui_ce, s_lane_cce[i]) != s_lane_cce[i])
            draw_playbar(i, px, pw, st, v);
    }
    // native-speed badge: "1:1" pops in the corner OPPOSITE the numeral
    // (mirror layout: track 1 right, track 2 left)
    {
        int nat = (v->cur_rate == 1.0f) ? 1 : 0;
        if (nat != s_lane_native[i]){
            s_lane_native[i] = nat;
            _bg = PANEL_BG;
            TFT_fillRect(i == 1 ? px + 4 : px + pw - 40, PANEL_Y + 6, 36,
                         TFT_getfontheight() + 2, PANEL_BG);
            if (nat){
                _fg = ACCENT;
                TFT_print("1:1", i == 1 ? px + 8 : px + pw - 36, PANEL_Y + 8);
            }
            _bg = TFT_BLACK;
        }
    }
    // position marker inside the playbar — a WHITE line over the grey waveform
    // (a white-on-white waveform swallowed it). The erase repaints its slice
    // from the SAME cell geometry the bar was drawn with — a float-based
    // uniform erase disagreed with the cell-drawn bar by a few px and chewed
    // flickering notches into the loop-point edges every marker pass
    // ("loop points jump around").
    if (v->play_len){
        int by = PANEL_Y + PANEL_H - 2 * PANEL_BAR_H - 4;
        int bx = px + 3, bw = pw - 6;
        int x = bx + PB_BW + 1 +
                (int)((uint64_t)(v->rpos_i < v->play_len ? v->rpos_i : v->play_len)
                      * (uint32_t)(bw - 2 * PB_BW - 6) / v->play_len);
        if (x != s_lane_barx[i]){
            if (s_lane_barx[i] > 0)           // restore black + wf + bracket slice
                playbar_slice(i, px, pw, st, v, s_lane_barx[i], 4);
            TFT_fillRect(x, by + PB_BW, 4, PANEL_BAR_H - 2 * PB_BW, (color_t){245, 245, 245});
            s_lane_barx[i] = x;
        }
    }
}

static void draw_banner(void){
    // one unmissable line: recording > sync-pending > armed > failed > pickup
    int st = 0;
    if (recording_is_active()) st = s3.rec_stop_wait ? 6 : 3;
    else if (s3.rec_wait_vid >= 0) st = 5;
    else if (s3.arm_target >= 0) st = (s3.ci.clk.locked || s3.int_bpm > 0.5f) ? 7 : 2;
    else if (s3.save_failed) st = 4;
    else if (s3.last_rec[0]) st = 1;
    if (st == s_banner_state && st != 3) return;   // REC repaints (name may arrive)
    s_banner_state = st;
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK;
    TFT_fillRect(0, BANNER_Y, _width, fh + 6, _bg);
    char b[64];
    switch (st){
        case 6:
            snprintf(b, sizeof(b), "REC V%d — stops on the beat",
                     recording_get_target_vid() + 1);
            _fg = COL_REC; break;
        case 5:
            snprintf(b, sizeof(b), "SYNCED — starts on the pulse (V%d)",
                     s3.rec_wait_vid + 1);
            _fg = COL_ARM; break;
        case 3:
            snprintf(b, sizeof(b), "REC -> V%d  (gate stops)", recording_get_target_vid() + 1);
            _fg = COL_REC; break;
        case 7:
            snprintf(b, sizeof(b), "ARMED V%d +CLK  (gate records on pulse)",
                     s3.arm_target + 1);
            _fg = COL_ARM; break;
        case 2:
            snprintf(b, sizeof(b), "ARMED V%d  (gate records)", s3.arm_target + 1);
            _fg = COL_ARM; break;
        case 4:
            snprintf(b, sizeof(b), "SAVE FAILED — take not loaded");
            _fg = COL_REC; break;
        case 1:
            snprintf(b, sizeof(b), "picked up: %s", s3.last_rec);
            _fg = (color_t){110, 110, 110}; break;
        default: return;
    }
    TFT_print(b, 8, BANNER_Y + 2);
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Sampler", 6, 4);
    s_last_dbpm = -1;           // corner = external clock tempo (panels carry
    draw_clock_bpm();           // their own numerals now)
    s_banner_state = -1;
    s_last_sel = s_voice_sel;
    for (int i = 0; i < S3_NVOICES; i++) { s_lane_state[i] = -1; draw_lane(i, true); }
    draw_banner();
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn:voice press:load hold:setup TRhold:arm", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int s3_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_FAST:
        case EV_TIMER_REPEATING_SLOW:
            if (s_last_sel != s_voice_sel) { live_full_redraw(); break; }
            for (int i = 0; i < S3_NVOICES; i++) draw_lane(i, false);
            draw_clock_bpm();
            draw_banner();
            if (event == EV_TIMER_REPEATING_SLOW){
                // starves + heal/retrig handshake + the FOCUSED voice's
                // cursor vs stream head — the starve-wedge post-mortem kit
                s3_voice_t *dv = &s3.v[s_voice_sel];
                // crop-start jitter over the last tick, in per-mille of the
                // take — the "loop points jumpy" hard number
                float jit = dv->ui_cs_max - dv->ui_cs_min;
                dv->ui_cs_min = 1.0f;
                dv->ui_cs_max = 0;
                char dbg[56];
                snprintf(dbg, sizeof(dbg), "%c%c s%lu/%lu h%lu/%lu d%lu j%d p%lu",
                         s3.v[0].playing ? 'P' : 's', s3.v[1].playing ? 'P' : 's',
                         (unsigned long)s3.v[0].dbg_starve,
                         (unsigned long)s3.v[1].dbg_starve,
                         (unsigned long)dv->dbg_heal,
                         (unsigned long)dv->dbg_retrig,
                         (unsigned long)recording_get_drops(),
                         (int)(jit > 0 ? jit * 1000.0f + 0.5f : 0),
                         (unsigned long)dv->rpos_i);
                audio_status_set_voices("s3", dbg);
            }
            break;
        case EV_FWD:
        case EV_BWD:
            // turn = toggle lane focus; an armed track DISARMS on the way
            // (Arlo: the only exit from arm used to be recording or a trip
            // to the Record page)
            if (s3.arm_target >= 0 && !recording_is_active())
                s3_toggle_arm(s3.arm_target);
            s_voice_sel = 1 - s_voice_sel;      // two lanes: turn toggles focus
            live_full_redraw();
            break;
        case EV_SHORT_PRESS:
            s_load_ret = M_S3_LIVE;
            return M_S3_LOAD;
        case EV_LONG_PRESS: return M_S3_SETUP;
        default: break;
    }
    return 0;
}

// ---- Setup --------------------------------------------------------------------
// Standard scrollable settings list on the shared setup-menu framework.
// Row grammar (Arlo, 2026-07-12): small option sets flip right on the click
// (TOGGLE); numeric ranges keep click-to-edit + turn with a [ value ] bracket.
static const setup_item_t s3_setup_items[] = {
    {"Voice",    ST_TOGGLE}, {"Mode",     ST_TOGGLE}, {"Reverse",  ST_TOGGLE},
    {"Crop",     ST_TOGGLE}, {"Speed CV", ST_TOGGLE}, {"Start CV", ST_TOGGLE},
    {"Len CV",   ST_TOGGLE}, {"Level",    ST_RANGE},  {"Pan",      ST_RANGE},
    {"Start",    ST_RANGE},  {"Length",   ST_RANGE},  {"Record",   ST_ACTION},
};
#define S3_SETUP_N 12

static void src_name(int src, char *val, size_t n){
    if (src < 0) snprintf(val, n, "off");
    else snprintf(val, n, "CV%d", src + 1);
}

static void setup_value_str(int i, char *val, size_t n){
    s3_voice_t *v = &s3.v[s_voice_sel];
    switch(i){
        case 0: snprintf(val, n, "%d: %.10s", s_voice_sel + 1,
                         v->name[0] ? v->name : "(empty)"); break;
        case 1: snprintf(val, n, "%s",
                         v->playmode == S3_MODE_LOOP ? "LOOP" : "ONE-SHOT"); break;
        case 2: snprintf(val, n, "%s", v->reverse ? "ON" : "OFF"); break;
        case 3:
            if (v->crop_mode == S3_CROP_OFF) snprintf(val, n, "OFF");
            else if (v->crop_mode == S3_CROP_QUANT)
                snprintf(val, n, v->bpm > 20.0f ? "QUANT" : "QUANT (no bpm)");
            else if (v->crop_mode == S3_CROP_QUANT2)
                snprintf(val, n, v->bpm > 20.0f ? "QUANTx2" : "QUANTx2 (no bpm)");
            else snprintf(val, n, "FREE");
            break;
        case 4: src_name(v->src_speed, val, n); break;
        case 5: src_name(v->src_start, val, n); break;
        case 6: src_name(v->src_len, val, n); break;
        case 7: snprintf(val, n, "%d%%", (int)(v->level * 100 + 0.5f)); break;
        case 8: snprintf(val, n, "%+d", (int)(v->pan * 100 + 0.5f)); break;
        case 9: snprintf(val, n, "%d%%", (int)(v->crop_start * 100 + 0.5f)); break;
        case 10: snprintf(val, n, "%d%%", (int)(v->crop_len * 100 + 0.5f)); break;
        case 11: snprintf(val, n, "%s", s3.arm_target >= 0 ? "ARMED" : "open"); break;
        default: val[0] = 0;
    }
}

// cycle a CV source: off -> CV1 -> ... -> CV8 -> off
static int src_cycle(int src, int dir){
    src += dir;
    if (src < -1) src = 7;
    if (src > 7) src = -1;
    return src;
}

static void setup_adj(int i, int dir){
    s3_voice_t *v = &s3.v[s_voice_sel];
    switch(i){
        case 0: s_voice_sel = 1 - s_voice_sel; break;
        case 1: v->playmode = v->playmode == S3_MODE_LOOP ? S3_MODE_ONESHOT : S3_MODE_LOOP; break;
        case 2: s3_set_reverse(s_voice_sel, !v->reverse); break;
        case 3:
            v->crop_mode = (v->crop_mode + (dir > 0 ? 1 : 3)) % 4;
            v->q_cs = 0;               // selector indices are mode-specific:
            v->q_ln = 1 << 20;         // start fresh (clamps into range)
            break;
        case 4: v->src_speed = src_cycle(v->src_speed, dir); break;
        case 5: v->src_start = src_cycle(v->src_start, dir); break;
        case 6: v->src_len   = src_cycle(v->src_len, dir); break;
        case 7: {
            float lv = v->level + dir * 0.05f;
            if (lv < 0) lv = 0;
            if (lv > 1.0f) lv = 1.0f;
            v->level = lv;
            break;
        }
        case 8: {
            float pn = v->pan + dir * 0.1f;
            if (pn < -1.0f) pn = -1.0f;
            if (pn > 1.0f) pn = 1.0f;
            v->pan = pn;
            break;
        }
        // crop is ENGINE-side: live, no head rebuild, no menu flicker.
        // Start slides the WHOLE window (sampler2 semantics) — length is
        // preserved; the engine lets it give way only at EOF.
        case 9: {
            float cs = v->crop_start + dir * 0.01f;
            if (cs < 0) cs = 0;
            if (cs > 0.98f) cs = 0.98f;
            v->crop_start = cs;
            break;
        }
        case 10: {
            float cl = v->crop_len + dir * 0.01f;
            if (cl > 1.0f) cl = 1.0f;
            if (cl < 0.02f) cl = 0.02f;
            v->crop_len = cl;
            break;
        }
    }
}

// Record row (last item) opens the Record page; Voice/Crop/CV cycles and the
// numeric ranges are all driven by setup_adj/setup_value_str above. A Voice
// switch changes every row's value — the framework repaints the whole list on
// each event, so no special-case redraw is needed here.
static int s3_setup_action(int i){
    if (i == S3_SETUP_N - 1) return M_S3_REC;   // Record -> Record page
    return 0;
}

static setup_menu_t s3_setup = {
    .items = s3_setup_items,
    .n = S3_SETUP_N,
    .title = "Sampler Setup",
    .aff_label = "Machine", .aff_target = M_MORE,
    .live_target = M_S3_LIVE,
    .render = setup_value_str, .adjust = setup_adj, .action = s3_setup_action,
};

static int s3_setup_handler(int it_id, int event, void *ev_data){
    (void)it_id; (void)ev_data;
    return setup_menu_event(&s3_setup, event);
}

// ---- Load browser: the shared two-level widget (folders -> big-name list) --
static int s3_load_handler(int it_id, int event, void *ev_data){
    if (event == EV_ENTERED_MENU){
        char t[24];
        snprintf(t, sizeof(t), "Load -> V%d", s_voice_sel + 1);
        sample_browser_enter(true, t, s3.v[s_voice_sel].name);
        return 0;
    }
    int r = sample_browser_event(event);
    if (r == 1){
        const char *sel = sample_browser_selected();
        if (strcmp(sel, s3.v[s_voice_sel].name) != 0)
            s3_load_sample(s_voice_sel, (char*)sel);
        return s_load_ret;
    }
    if (r == 2) return s_load_ret;   // escape without loading
    return 0;
}

// ---- Record ---------------------------------------------------------------------
static const char *rec_labels[] = {"Arm V1", "Arm V2", "Monitor", "Arm mutes",
                                   "Int Clock", "Clock PPQ", "Clock Src"};
#define S3_REC_N 7

// PPQ ladder — click cycles; applies to the external detector AND the
// internal clock's pulse synthesis (Arlo's jig clocks at 8)
static const float ppq_ladder[] = {1.0f, 2.0f, 4.0f, 8.0f};
#define PPQ_N 4

static void rec_redraw(int pos){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE;
    TFT_print("Record", 6, 4);
    for (int i = 0; i < S3_REC_N; i++){
        int y = fh + 16 + i * (fh + 8);
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
        TFT_print((char*)rec_labels[i], 8, y);
        char val[24];
        if (i < 2) snprintf(val, sizeof(val), "%s", s3.arm_target == i ? "ARMED" : "-");
        else if (i == 2) snprintf(val, sizeof(val), "%s", s3.monitor ? "ON" : "OFF");
        else if (i == 3) snprintf(val, sizeof(val), "%s", s3.arm_mutes ? "ON" : "OFF");
        else if (i == 4){
            if (s3.int_bpm > 0.5f) snprintf(val, sizeof(val), "%.0f bpm%s",
                                            s3.int_bpm, s3.ci.clk.locked ? " (ext!)" : "");
            else snprintf(val, sizeof(val), "OFF");
        }
        else if (i == 5) snprintf(val, sizeof(val), "%d", (int)(s3.ci.ppb + 0.5f));
        else snprintf(val, sizeof(val), "%s", clock_source_name(s3.clk_src));
        if (i < 2 && s3.arm_target == i) _fg = COL_ARM;
        TFT_print(val, _width - TFT_getStringWidth(val) - 10, y);
    }
    int y = fh + 16 + S3_REC_N * (fh + 8) + 8;
    _bg = TFT_BLACK;
    if (recording_is_active()){
        _fg = COL_REC;
        char b[40];
        snprintf(b, sizeof(b), "RECORDING -> V%d", recording_get_target_vid() + 1);
        TFT_print(b, 8, y);
    } else if (s3.save_failed){
        _fg = COL_REC;
        TFT_print("last take: SAVE FAILED", 8, y);
    } else if (s3.last_rec[0]){
        _fg = (color_t){110, 110, 110};
        char b[40];
        snprintf(b, sizeof(b), "last take: %.16s", s3.last_rec);
        TFT_print(b, 8, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("armed voice: its gate starts/stops the take", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int s3_rec_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;      // sel: editing the Int Clock bpm
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; rec_redraw(pos); break;
        case EV_TIMER_REPEATING_SLOW: if (!sel) rec_redraw(pos); break;  // live REC status
        case EV_FWD:
        case EV_BWD: {
            int dir = (event == EV_FWD) ? +1 : -1;
            if (sel) {                 // adjusting internal bpm
                float b = s3.int_bpm < 0.5f ? 120.0f : s3.int_bpm + dir;
                if (b < 40) b = 40;
                if (b > 240) b = 240;
                s3.int_bpm = b;
            } else {
                pos = (pos + S3_REC_N + dir) % S3_REC_N;
            }
            rec_redraw(pos);
            break;
        }
        case EV_SHORT_PRESS:
            if (pos < 2) s3_toggle_arm(pos);
            else if (pos == 2) s3.monitor = !s3.monitor;
            else if (pos == 3) s3.arm_mutes = !s3.arm_mutes;
            else if (pos == 5) {       // Clock PPQ: click cycles the ladder
                int k = 0;
                for (int i = 0; i < PPQ_N; i++)
                    if (s3.ci.ppb >= ppq_ladder[i] - 0.1f &&
                        s3.ci.ppb <= ppq_ladder[i] + 0.1f) k = i;
                clockin_set_ppb(&s3.ci, ppq_ladder[(k + 1) % PPQ_N]);
            }
            else if (pos == 6) {       // Clock Src: CV1..8 + AUDIO (trigs are
                                       // voice gates — never clock inputs here)
                s3.clk_src = clock_source_cycle_cv_audio(s3.clk_src, +1);
                if (s3.clk_src == CLK_SRC_AUDIO && beatlisten_get_mode() == BL_OFF) {
                    beatlisten_set_mode(BL_GROOVE);   // AUDIO implies the ear is on
                    configSetIntSetting("blisten", BL_GROOVE);
                }
            }
            else {                     // Int Clock: press = edit bpm / press again = done;
                if (sel) sel = 0;      // (turn OFF by dialing... hold = off below)
                else { sel = 1; if (s3.int_bpm < 0.5f) s3.int_bpm = 120.0f; }
            }
            rec_redraw(pos);
            break;
        case EV_LONG_PRESS:
            if (pos == 4 && s3.int_bpm > 0.5f) {   // hold on the row = clock OFF
                s3.int_bpm = 0;
                sel = 0;
                rec_redraw(pos);
                break;
            }
            return M_S3_LIVE;
        default: break;
    }
    return 0;
}

// ---- registration -----------------------------------------------------------------
static void s3_register_pages(void *menusys){
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_S3_LIVE);  menusys_item_set_default_cb(_ms, M_S3_LIVE, s3_live_handler);
    menusys_new_item(_ms, M_S3_SETUP); menusys_item_set_default_cb(_ms, M_S3_SETUP, s3_setup_handler);
    menusys_new_item(_ms, M_S3_LOAD);  menusys_item_set_default_cb(_ms, M_S3_LOAD, s3_load_handler);
    menusys_new_item(_ms, M_S3_REC);   menusys_item_set_default_cb(_ms, M_S3_REC, s3_rec_handler);
}

static int s3_main_event(int event, void *ev_data){
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW){
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        _fg = ACCENT;
        char s[64];
        snprintf(s, sizeof(s), "Sampler: %.10s / %.10s",
                 s3.v[0].name[0] ? s3.v[0].name : "-",
                 s3.v[1].name[0] ? s3.v[1].name : "-");
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const s3_main_items[] = {"Live", "Setup", "Record"};
static const int s3_main_targets[] = {M_S3_LIVE, M_S3_SETUP, M_S3_REC};

const machine_ui_t s3_menu_ui = {
    .main_items = s3_main_items,
    .main_targets = s3_main_targets,
    .n_main = 3,
    .register_pages = s3_register_pages,
    .main_event = s3_main_event,
    .boot_target = M_S3_LIVE,
};
