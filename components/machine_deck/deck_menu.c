// Deck UI — Live (track, BPMs, rate, beat flash, position bar; press =
// play/pause) and Setup (track/sync/clock/mult-div/loop/BPM/analyze) +
// the shared library browser for track selection.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "menusys.h"
#include "menu_types.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "audio.h"
#include "sample_ram.h"
#include "deck_priv.h"
#include "esp_timer.h"

static const color_t ACCENT = {40, 200, 230};
static const color_t BEAT   = {240, 200, 40};

static char (*s_samples)[24] = NULL;
static int  s_n_samples = 0, s_sample_idx = 0;

static void refresh_samples(void){
    s_n_samples = sample_list_shared(&s_samples);
    s_sample_idx = 0;
    for (int i = 0; i < s_n_samples; i++)
        if (strcmp(s_samples[i], dk.track) == 0) { s_sample_idx = i; break; }
}

// ---- Live -------------------------------------------------------------------
// Redraw discipline: everything is change-driven. Full-region repaints every
// timer tick made the whole screen strobe (Arlo, first hardware test).
static int s_last_beat = -1;
static int s_last_barx = -1;
static bool s_beat_lit = false;
static char s_info1[64] = "", s_info2[64] = "";
static int s_last_dbpm = -1;

// the number that matters, BIG: the tempo actually playing right now
// (track bpm x current rate = the external tempo when locked)
static void draw_big_bpm(void){
    // locked: show the TARGET tempo (steady) — the instantaneous rate
    // legitimately wobbles a few % while the PLL corrects phase, which made
    // the big number dance even with a perfect clock
    float bpm;
    if (dk.sync && dk.clk.locked && dk.clk.bpm > 0)
        bpm = dk.clk.bpm / dk_ppb[dk.ppb_idx] * dk.speed_mult;
    else
        bpm = dk.track_bpm > 0 ? dk.track_bpm * dk.rate : 0;
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
    _fg = (dk.sync && dk.clk.locked) ? (color_t){40, 200, 90} : TFT_WHITE;
    TFT_print(s, _width - TFT_getStringWidth(s) - 8, 4);
    cfont = f;
}

static void draw_info(void){
    int fh = TFT_getfontheight();
    int y = fh + 42;
    char s1[64], s2[64];
    snprintf(s1, sizeof(s1), "trk %.1f  %s  x%s  %s",
             dk.track_bpm, dk.playing ? (dk.loading ? "BUF" : "PLAY") : "STOP",
             dk.speed_mult == 0.5f ? ".5" : (dk.speed_mult == 2.0f ? "2" : "1"),
             dk.flt_mode == 1 ? "LP" : (dk.flt_mode == 2 ? "HP" : ""));
    if (dk.sync){
        if (dk.clk.locked) snprintf(s2, sizeof(s2), "ext %.1f bpm  LOCK  err %+d%%",
                                    dk.clk.bpm / dk_ppb[dk.ppb_idx], (int)(dk.phase_err * 100));
        else snprintf(s2, sizeof(s2), "ext: waiting for clock on CV%d", dk.clk_src + 1);
    } else s2[0] = 0;
    if (strcmp(s1, s_info1) != 0){
        strcpy(s_info1, s1);
        _bg = TFT_BLACK; TFT_fillRect(0, y, _width, fh + 4, _bg);
        _fg = TFT_WHITE;
        TFT_print(s1, 8, y);
    }
    if (strcmp(s2, s_info2) != 0){
        strcpy(s_info2, s2);
        _bg = TFT_BLACK; TFT_fillRect(0, y + fh + 6, _width, fh + 4, _bg);
        _fg = dk.clk.locked ? (color_t){40, 200, 90} : TFT_LIGHTGREY;
        if (s2[0]) TFT_print(s2, 8, y + fh + 6);
    }
}

// big transport: full-width-ish bar right under the info lines, beat lamp
// beside it (was a 12px sliver at the bottom — too small to perform with)
#define TBAR_X 8
#define TBAR_Y 112
#define TBAR_H 30
#define TBAR_W (_width - 64)

static void draw_posbar_frame(void){
    _bg = (color_t){20, 22, 30};
    TFT_fillRect(TBAR_X, TBAR_Y, TBAR_W, TBAR_H, _bg);
    _fg = (color_t){70, 70, 90};
    TFT_drawRect(TBAR_X, TBAR_Y, TBAR_W, TBAR_H, _fg);
    s_last_barx = -1;
}

static void draw_posbar(void){
    if (!dk.file_frames) return;
    int x = TBAR_X + 2 + (int)((uint64_t)dk.rpos_i * (TBAR_W - 8) / dk.file_frames);
    if (x == s_last_barx) return;
    if (s_last_barx > 0)                      // erase only the old marker slice
        TFT_fillRect(s_last_barx, TBAR_Y + 1, 5, TBAR_H - 2, (color_t){20, 22, 30});
    TFT_fillRect(x, TBAR_Y + 1, 5, TBAR_H - 2, ACCENT);
    s_last_barx = x;
}

static void draw_beat(bool on){
    if (on == s_beat_lit) return;             // change-driven only
    s_beat_lit = on;
    color_t c = on ? BEAT : (color_t){30, 30, 36};
    TFT_fillCircle(_width - 28, TBAR_Y + TBAR_H / 2, 15, c);
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Deck", 6, 4);
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    char nm[16];
    snprintf(nm, sizeof(nm), "%.9s", dk.track[0] ? dk.track : "(none)");
    TFT_print(nm, 8, TFT_getfontheight() + 4);
    cfont = f;
    s_last_dbpm = -1;
    draw_big_bpm();
    s_info1[0] = 0;
    s_info2[0] = 0;
    s_beat_lit = true;      // force the first draw_beat(false) to paint
    draw_info();
    draw_posbar_frame();
    draw_posbar();
    draw_beat(false);
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn:scrub press:play TR1:go TR2:stop k6:filt k7:x2", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
    s_last_beat = -1;
}

// A fast run of detents (real spinning) still jumps a bar each; a lone or
// slow detent instead reads as a tiny pitch-bend nudge. Classified purely by
// the gap since the last detent — no separate "spin" state to fall out of
// sync with the encoder.
#define DK_SPIN_GAP_US 130000
static int64_t s_last_detent_us = 0;

static void deck_scrub_or_nudge(int dir){
    int64_t now = esp_timer_get_time();
    bool fast = (now - s_last_detent_us) < DK_SPIN_GAP_US;
    s_last_detent_us = now;
    if (fast) deck_seek_beats(dir * 4);   // scrub one bar per detent
    else      deck_nudge(dir);
}

static int deck_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_FAST:
        case EV_TIMER_REPEATING_SLOW: {
            draw_big_bpm();
            draw_info();
            draw_posbar();
            if (dk.track_bpm > 20 && dk.playing){
                uint32_t beat_tf = (uint32_t)(60.0f * DK_RATE / dk.track_bpm);
                int beat = (int)(((int64_t)dk.rpos_i - (int64_t)dk.grid_offset) / (int64_t)beat_tf);
                if (beat != s_last_beat){ draw_beat(true); s_last_beat = beat; }
                else draw_beat(false);
            } else draw_beat(false);
            if (event == EV_TIMER_REPEATING_SLOW){
                // engine internals through /status (v1) for remote debugging
                char dbg[32];
                snprintf(dbg, sizeof(dbg), "%c e%lu i%lu p%lu E%+d",
                         dk.playing ? 'P' : 's',
                         (unsigned long)dk.dbg_edges,
                         (unsigned long)(dk.dbg_iv / 44),        // ms between fires
                         (unsigned long)(dk.clk.period / 44),    // ms accepted period
                         (int)(dk.phase_err * 100));             // PLL convergence
                audio_status_set_voices("deck", dbg);
            }
            break;
        }
        case EV_SHORT_PRESS: deck_toggle_play(); break;
        // Fast spin (detents arriving quickly) still scrubs a bar at a time;
        // a lone/slow detent instead gives a tiny DJ pitch-bend nudge — the
        // encoder itself decides which by how fast the clicks are coming in
        case EV_FWD: deck_scrub_or_nudge(+1); break;
        case EV_BWD: deck_scrub_or_nudge(-1); break;
        case EV_LONG_PRESS: return M_MAIN;
        default: break;
    }
    return 0;
}

// ---- Setup --------------------------------------------------------------------
static const char *setup_labels[] = {"Track", "Sync", "Clock Src", "Clock", "Loop", "BPM", "Grid Nudge", "Analyze"};
#define DK_SETUP_N 8

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE;
    TFT_print("Deck Setup", 6, 4);
    for (int i = 0; i < DK_SETUP_N; i++){
        int y = fh + 14 + i * (fh + 7);
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 5, _bg);
        TFT_print((char*)setup_labels[i], 8, y);
        char v[28];
        switch(i){
            case 0: snprintf(v, sizeof(v), "%s", dk.track[0] ? dk.track : "(none)"); break;
            case 1: snprintf(v, sizeof(v), "%s", dk.sync ? "ON" : "OFF"); break;
            case 2: snprintf(v, sizeof(v), "CV%d", dk.clk_src + 1); break;
            case 3: snprintf(v, sizeof(v), "%s", dk_ppb_names[dk.ppb_idx]); break;
            case 4: snprintf(v, sizeof(v), "%s", dk.loop ? "ON" : "OFF"); break;
            case 5:
                if (dk.track_bpm > 0) snprintf(v, sizeof(v), "%.1f", dk.track_bpm);
                else snprintf(v, sizeof(v), "?");
                break;
            case 6: snprintf(v, sizeof(v), "%lums", (unsigned long)(dk.grid_offset * 1000 / DK_RATE)); break;
            case 7:
                if (dk.an_state == DK_AN_RUNNING) snprintf(v, sizeof(v), "%d%%", dk.an_progress);
                else if (dk.an_state == DK_AN_DONE) snprintf(v, sizeof(v), "%.1f bpm", dk.an_bpm);
                else if (dk.an_state == DK_AN_FAIL) snprintf(v, sizeof(v), "FAILED");
                else snprintf(v, sizeof(v), "press");
                break;
        }
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("Analyze finds BPM + beat grid; cached per track", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void setup_adj(int i, int dir){
    switch(i){
        case 1: dk.sync = !dk.sync; break;
        case 2: dk.clk_src = (dk.clk_src + (dir > 0 ? 1 : 7)) & 7; break;
        case 3: dk.ppb_idx += dir; if (dk.ppb_idx < 0) dk.ppb_idx = 0; if (dk.ppb_idx > 4) dk.ppb_idx = 4; break;
        case 4: dk.loop = !dk.loop; break;
        case 5: {
            float b = dk.track_bpm > 0 ? dk.track_bpm : 120.0f;
            b += dir * 0.5f;
            if (b < 40) b = 40;
            if (b > 240) b = 240;
            dk.track_bpm = b;
            break;
        }
        case 6: {
            int32_t g = (int32_t)dk.grid_offset + dir * (DK_RATE / 100);   // ±10 ms
            if (g < 0) g = 0;
            dk.grid_offset = (uint32_t)g;
            break;
        }
    }
}

static int deck_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; setup_redraw(pos, sel); break;
        case EV_TIMER_REPEATING_SLOW:
            if (dk.an_state == DK_AN_RUNNING || dk.an_state == DK_AN_DONE) setup_redraw(pos, sel);
            break;
        case EV_FWD:
            if(sel) setup_adj(pos, +1);
            else pos = (pos + 1) % DK_SETUP_N;
            setup_redraw(pos, sel);
            break;
        case EV_BWD:
            if(sel) setup_adj(pos, -1);
            else pos = (pos + DK_SETUP_N - 1) % DK_SETUP_N;
            setup_redraw(pos, sel);
            break;
        case EV_SHORT_PRESS:
            if(pos == 0){ refresh_samples(); return M_DECK_LOAD; }
            if(pos == 7){ deck_analyze_start(); setup_redraw(pos, sel); break; }
            sel = !sel; setup_redraw(pos, sel);
            break;
        case EV_LONG_PRESS: return M_MAIN;
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
    snprintf(h, sizeof(h), "Load Track  (%d/%d)", s_n_samples ? s_sample_idx + 1 : 0, s_n_samples);
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

static int deck_load_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: load_redraw(); break;
        case EV_FWD: if(s_n_samples){ s_sample_idx = (s_sample_idx + 1) % s_n_samples; load_redraw(); } break;
        case EV_BWD: if(s_n_samples){ s_sample_idx = (s_sample_idx + s_n_samples - 1) % s_n_samples; load_redraw(); } break;
        case EV_SHORT_PRESS:
            if(s_n_samples) deck_load_track(s_samples[s_sample_idx]);
            return M_DECK_SETUP;
        case EV_LONG_PRESS:
            return M_DECK_SETUP;
        default: break;
    }
    return 0;
}

// ---- registration -----------------------------------------------------------------
static void deck_register_pages(void *menusys){
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_DECK_LIVE);  menusys_item_set_default_cb(_ms, M_DECK_LIVE, deck_live_handler);
    menusys_new_item(_ms, M_DECK_SETUP); menusys_item_set_default_cb(_ms, M_DECK_SETUP, deck_setup_handler);
    menusys_new_item(_ms, M_DECK_LOAD);  menusys_item_set_default_cb(_ms, M_DECK_LOAD, deck_load_handler);
}

static int deck_main_event(int event, void *ev_data){
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW){
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        _fg = ACCENT;
        char s[64];
        snprintf(s, sizeof(s), "Deck: %.12s %s %.0f bpm",
                 dk.track[0] ? dk.track : "(none)",
                 dk.playing ? "playing" : "stopped", dk.track_bpm);
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const deck_main_items[] = {"Live", "Setup"};
static const int deck_main_targets[] = {M_DECK_LIVE, M_DECK_SETUP};

const machine_ui_t deck_menu_ui = {
    .main_items = deck_main_items,
    .main_targets = deck_main_targets,
    .n_main = 2,
    .register_pages = deck_register_pages,
    .main_event = deck_main_event,
    .boot_target = M_DECK_LIVE,
};
