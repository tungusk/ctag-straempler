// Sampler3 UI — Live (two voice lanes + explicit ARM/REC banner; turn =
// select voice, press = load browser, long = Setup), Setup (per-voice
// mode/reverse/pitch-source/level/pan/trim + Record page entry), Load
// (usr/*.RAW browser -> selected voice), Record (explicit arm target +
// monitor; the silent short-press arm-cycle of the old sampler is gone —
// that hidden state read as "sampler only plays monitor audio").
#include <stdio.h>
#include <string.h>
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
#include "sampler3_priv.h"

static const color_t ACCENT   = {230, 160, 40};
static const color_t COL_REC  = {220, 40, 40};
static const color_t COL_ARM  = {230, 200, 40};
static const color_t COL_PLAY = {25, 120, 50};
static const color_t COL_IDLE = {30, 60, 140};

static int s_voice_sel = 0;                    // lane focus on Live / Setup target
static char (*s_samples)[S3_NAME_LEN] = NULL;
static int  s_n_samples = 0, s_sample_idx = 0;
static int  s_load_ret = M_S3_LIVE;

static void refresh_samples(void){
    s_n_samples = s3_list_samples(&s_samples);
    s_sample_idx = 0;
    for (int i = 0; i < s_n_samples; i++)
        if (strcmp(s_samples[i], s3.v[s_voice_sel].name) == 0) { s_sample_idx = i; break; }
}

// ---- Live ---------------------------------------------------------------------
// change-driven redraws only (full-region repaints strobe — deck lesson)

// big lanes: DEJAVU24 name + a tall bar whose BACKGROUND is the state —
// blue idle, green playing, yellow armed, red recording (the deck's
// "bar color IS the transport" lesson, doubled)
#define LANE_Y(i)   (26 + (i) * 66)
#define LANE_BAR_H  24
#define BANNER_Y    162

static int s_lane_barx[S3_NVOICES] = {-1, -1};
static char s_lane_line[S3_NVOICES][48] = {{0}, {0}};
static int s_lane_state[S3_NVOICES] = {-1, -1};
static int s_banner_state = -1;
static int s_last_sel = -1;

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

static void draw_lane(int i, bool full){
    s3_voice_t *v = &s3.v[i];
    int y = LANE_Y(i);
    int st = lane_state(i);
    char line[48];
    snprintf(line, sizeof(line), "%.11s %s%s%s",
             v->name[0] ? v->name : "(empty)",
             v->playmode == S3_MODE_LOOP ? "LP" : "1S",
             v->reverse ? "R" : "",
             i == s_voice_sel ? "\x01" : "");     // \x01 = selection slot marker
    if (full || strcmp(line, s_lane_line[i]) != 0){
        strcpy(s_lane_line[i], line);
        Font f = cfont;
        TFT_setFont(DEJAVU24_FONT, NULL);
        int bfh = TFT_getfontheight();
        _bg = TFT_BLACK;
        TFT_fillRect(0, y, _width, bfh + 2, _bg);
        _fg = (i == s_voice_sel) ? TFT_WHITE : TFT_LIGHTGREY;
        char nm[48];
        snprintf(nm, sizeof(nm), "%.11s %s%s",
                 v->name[0] ? v->name : "(empty)",
                 v->playmode == S3_MODE_LOOP ? "LP" : "1S",
                 v->reverse ? "R" : "");
        TFT_print(nm, 6, y);
        if (i == s_voice_sel){                    // selection marker, RIGHT side
            _fg = ACCENT;
            TFT_print("<", _width - TFT_getStringWidth("<") - 6, y);
        }
        cfont = f;
        s_lane_barx[i] = -1;
    }
    // tall state-colored bar
    int by = y + 28;
    if (st != s_lane_state[i] || s_lane_barx[i] < 0){
        s_lane_state[i] = st;
        TFT_fillRect(6, by, _width - 12, LANE_BAR_H, lane_bg(st));
        _fg = (color_t){70, 70, 90};
        TFT_drawRect(6, by, _width - 12, LANE_BAR_H, _fg);
        _bg = TFT_BLACK;
        s_lane_barx[i] = -1;
    }
    if (v->play_len){
        int x = 8 + (int)((uint64_t)(v->rpos_i < v->play_len ? v->rpos_i : v->play_len)
                          * (_width - 20) / v->play_len);
        if (x != s_lane_barx[i]){
            if (s_lane_barx[i] > 0)
                TFT_fillRect(s_lane_barx[i], by + 1, 5, LANE_BAR_H - 2, lane_bg(st));
            TFT_fillRect(x, by + 1, 5, LANE_BAR_H - 2, (color_t){235, 235, 235});
            s_lane_barx[i] = x;
        }
    }
}

static void draw_banner(void){
    // one unmissable line: recording > sync-pending > armed > failed > pickup
    int st = 0;
    if (recording_is_active()) st = s3.rec_stop_wait ? 6 : 3;
    else if (s3.rec_wait_vid >= 0) st = 5;
    else if (s3.arm_target >= 0) st = s3.clk.locked ? 7 : 2;
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
    _fg = ACCENT;
    {   // selected voice as a BIG corner numeral
        Font f = cfont;
        TFT_setFont(DEJAVU24_FONT, NULL);
        char hdr[4];
        snprintf(hdr, sizeof(hdr), "%d", s_voice_sel + 1);
        TFT_print(hdr, _width - TFT_getStringWidth(hdr) - 8, 2);
        cfont = f;
    }
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
            draw_banner();
            if (event == EV_TIMER_REPEATING_SLOW){
                char dbg[48];
                snprintf(dbg, sizeof(dbg), "%c%c s%lu/%lu",
                         s3.v[0].playing ? 'P' : 's', s3.v[1].playing ? 'P' : 's',
                         (unsigned long)s3.v[0].dbg_starve,
                         (unsigned long)s3.v[1].dbg_starve);
                audio_status_set_voices("s3", dbg);
            }
            break;
        case EV_FWD:
        case EV_BWD:
            s_voice_sel = 1 - s_voice_sel;      // two lanes: turn toggles focus
            live_full_redraw();
            break;
        case EV_SHORT_PRESS:
            refresh_samples();
            s_load_ret = M_S3_LIVE;
            return M_S3_LOAD;
        case EV_LONG_PRESS: return M_S3_SETUP;
        default: break;
    }
    return 0;
}

// ---- Setup --------------------------------------------------------------------
static const char *setup_labels[] = {"Voice", "Mode", "Reverse", "1V/oct",
                                     "CV6/7", "Level", "Pan", "Start", "Length",
                                     "Record"};
#define S3_SETUP_N 10

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE;
    TFT_print("Sampler Setup", 6, 4);
    menuTFTPrintAffordance("System", pos == -1);
    s3_voice_t *v = &s3.v[s_voice_sel];
    for (int i = 0; i < S3_SETUP_N; i++){
        int y = fh + 12 + i * (fh + 5);
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 3, _bg);
        TFT_print((char*)setup_labels[i], 8, y);
        char val[28];
        switch(i){
            case 0: snprintf(val, sizeof(val), "%d: %.10s", s_voice_sel + 1,
                             v->name[0] ? v->name : "(empty)"); break;
            case 1: snprintf(val, sizeof(val), "%s",
                             v->playmode == S3_MODE_LOOP ? "LOOP" : "ONE-SHOT"); break;
            case 2: snprintf(val, sizeof(val), "%s", v->reverse ? "ON" : "OFF"); break;
            case 3: snprintf(val, sizeof(val), "%s (CV%d)",
                             v->v1oct ? "ON" : "OFF", 1 + s_voice_sel); break;
            case 4: snprintf(val, sizeof(val), "%s (CV%d)",
                             v->cv67_dest == S3_CV67_SPEED ? "Speed" : "off",
                             6 + s_voice_sel); break;
            case 5: snprintf(val, sizeof(val), "%d%%", (int)(v->level * 100 + 0.5f)); break;
            case 6: snprintf(val, sizeof(val), "%+d", (int)(v->pan * 100 + 0.5f)); break;
            case 7: snprintf(val, sizeof(val), "%d%%", (int)(v->start_pct * 100 + 0.5f)); break;
            case 8: snprintf(val, sizeof(val), "%d%%", (int)(v->len_pct * 100 + 0.5f)); break;
            case 9: snprintf(val, sizeof(val), "%s",
                             s3.arm_target >= 0 ? "ARMED" : "open"); break;
        }
        TFT_print(val, _width - TFT_getStringWidth(val) - 10, y);
    }
}

static void setup_adj(int i, int dir){
    s3_voice_t *v = &s3.v[s_voice_sel];
    switch(i){
        case 0: s_voice_sel = 1 - s_voice_sel; break;
        case 1: v->playmode = v->playmode == S3_MODE_LOOP ? S3_MODE_ONESHOT : S3_MODE_LOOP; break;
        case 2: s3_set_window(s_voice_sel, v->start_pct, v->len_pct, !v->reverse); break;
        case 3: v->v1oct = !v->v1oct; break;
        case 4: v->cv67_dest = (v->cv67_dest == S3_CV67_SPEED) ? S3_CV67_OFF : S3_CV67_SPEED; break;
        case 5: {
            float lv = v->level + dir * 0.05f;
            if (lv < 0) lv = 0;
            if (lv > 1.0f) lv = 1.0f;
            v->level = lv;
            break;
        }
        case 6: {
            float pn = v->pan + dir * 0.1f;
            if (pn < -1.0f) pn = -1.0f;
            if (pn > 1.0f) pn = 1.0f;
            v->pan = pn;
            break;
        }
        case 7: s3_set_window(s_voice_sel, v->start_pct + dir * 0.01f, v->len_pct, v->reverse); break;
        case 8: s3_set_window(s_voice_sel, v->start_pct, v->len_pct + dir * 0.01f, v->reverse); break;
    }
}

static int s3_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; setup_redraw(pos, sel); break;
        case EV_FWD:
            if(sel) setup_adj(pos, +1);
            else { pos++; if(pos >= S3_SETUP_N) pos = -1; }
            setup_redraw(pos, sel);
            break;
        case EV_BWD:
            if(sel) setup_adj(pos, -1);
            else { pos--; if(pos < -1) pos = S3_SETUP_N - 1; }
            setup_redraw(pos, sel);
            break;
        case EV_SHORT_PRESS:
            if(pos == -1) return M_MORE;
            if(pos == S3_SETUP_N - 1) return M_S3_REC;
            sel = !sel; setup_redraw(pos, sel);
            break;
        case EV_LONG_PRESS: return M_S3_LIVE;
        default: break;
    }
    return 0;
}

// ---- Load browser ---------------------------------------------------------------
static void load_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; _fg = TFT_LIGHTGREY;
    char h[48];
    snprintf(h, sizeof(h), "Load -> V%d  (%d/%d)", s_voice_sel + 1,
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
    for (int k = 1; k <= 4; k++){
        int up = s_sample_idx - k, dn = s_sample_idx + k;
        int yup = cy - bigfh / 2 - k * (fh + 4) - 4;
        int ydn = cy + bigfh / 2 + (k - 1) * (fh + 4) + 6;
        if (up >= 0){ char *nn = s_samples[up]; TFT_print(nn, _width / 2 - TFT_getStringWidth(nn) / 2, yup); }
        if (dn < s_n_samples){ char *nn = s_samples[dn]; TFT_print(nn, _width / 2 - TFT_getStringWidth(nn) / 2, ydn); }
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    char *hint = "turn:browse  press:load  hold:cancel";
    TFT_print(hint, _width / 2 - TFT_getStringWidth(hint) / 2, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int s3_load_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: load_redraw(); break;
        case EV_FWD: if(s_n_samples){ s_sample_idx = (s_sample_idx + 1) % s_n_samples; load_redraw(); } break;
        case EV_BWD: if(s_n_samples){ s_sample_idx = (s_sample_idx + s_n_samples - 1) % s_n_samples; load_redraw(); } break;
        case EV_SHORT_PRESS:
            if(s_n_samples && strcmp(s_samples[s_sample_idx], s3.v[s_voice_sel].name) != 0)
                s3_load_sample(s_voice_sel, s_samples[s_sample_idx]);
            return s_load_ret;
        case EV_LONG_PRESS:
            return s_load_ret;      // escape without loading
        default: break;
    }
    return 0;
}

// ---- Record ---------------------------------------------------------------------
static const char *rec_labels[] = {"Arm V1", "Arm V2", "Monitor", "Arm mutes"};
#define S3_REC_N 4

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
        else snprintf(val, sizeof(val), "%s", s3.arm_mutes ? "ON" : "OFF");
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
    static int pos = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; rec_redraw(pos); break;
        case EV_TIMER_REPEATING_SLOW: rec_redraw(pos); break;   // live REC status
        case EV_FWD: pos = (pos + 1) % S3_REC_N; rec_redraw(pos); break;
        case EV_BWD: pos = (pos + S3_REC_N - 1) % S3_REC_N; rec_redraw(pos); break;
        case EV_SHORT_PRESS:
            if (pos < 2) s3_toggle_arm(pos);
            else if (pos == 2) s3.monitor = !s3.monitor;
            else s3.arm_mutes = !s3.arm_mutes;
            rec_redraw(pos);
            break;
        case EV_LONG_PRESS: return M_S3_LIVE;
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
