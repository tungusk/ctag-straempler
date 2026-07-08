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
#include "sample_ram.h"
#include "deck_priv.h"

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
static int s_last_beat = -1;
static int s_last_barx = -1;

static void draw_info(void){
    int fh = TFT_getfontheight();
    int y = fh + 42;
    _bg = TFT_BLACK; TFT_fillRect(0, y, _width, (fh + 6) * 2, _bg);
    _fg = TFT_WHITE;
    char s[64];
    snprintf(s, sizeof(s), "trk %.1f bpm  %s  rate %d%%",
             dk.track_bpm, dk.playing ? (dk.loading ? "BUF" : "PLAY") : "STOP",
             (int)(dk.rate * 100));
    TFT_print(s, 8, y);
    if (dk.sync){
        _fg = dk.clk.locked ? (color_t){40, 200, 90} : TFT_LIGHTGREY;
        if (dk.clk.locked) snprintf(s, sizeof(s), "ext %.1f bpm  LOCK  err %+d%%",
                                    dk.clk.bpm / dk_ppb[dk.ppb_idx], (int)(dk.phase_err * 100));
        else snprintf(s, sizeof(s), "ext: waiting for clock on CV%d", dk.clk_src + 1);
        TFT_print(s, 8, y + fh + 6);
    }
}

static void draw_posbar(void){
    int y = _height - 46;
    _bg = (color_t){20, 22, 30};
    TFT_fillRect(8, y, _width - 16, 12, _bg);
    _fg = (color_t){70, 70, 90};
    TFT_drawRect(8, y, _width - 16, 12, _fg);
    if (dk.file_frames){
        int x = 8 + (int)((uint64_t)dk.rpos_i * (_width - 16) / dk.file_frames);
        if (x != s_last_barx){
            TFT_fillRect(x - 1, y + 1, 3, 10, ACCENT);
            s_last_barx = x;
        }
    }
}

static void draw_beat(bool on){
    int fh = TFT_getfontheight();
    color_t c = on ? BEAT : (color_t){30, 30, 36};
    TFT_fillCircle(_width - 26, fh + 52, 12, c);
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Deck", 6, 4);
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    char nm[16];
    snprintf(nm, sizeof(nm), "%.12s", dk.track[0] ? dk.track : "(no track)");
    TFT_print(nm, 8, TFT_getfontheight() + 4);
    cfont = f;
    draw_info();
    draw_posbar();
    draw_beat(false);
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("press:play/stop  TR1:restart  knob7:rate(free)", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
    s_last_beat = -1;
    s_last_barx = -1;
}

static int deck_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_FAST:
        case EV_TIMER_REPEATING_SLOW: {
            draw_info();
            draw_posbar();
            if (dk.track_bpm > 20 && dk.playing){
                uint32_t beat_tf = (uint32_t)(60.0f * DK_RATE / dk.track_bpm);
                int beat = (int)(((int64_t)dk.rpos_i - (int64_t)dk.grid_offset) / (int64_t)beat_tf);
                if (beat != s_last_beat){ draw_beat((beat & 3) == 0 || beat != s_last_beat); s_last_beat = beat; }
                else draw_beat(false);
            }
            break;
        }
        case EV_SHORT_PRESS: deck_toggle_play(); break;
        case EV_FWD:  dk.grid_offset += DK_RATE / 100; break;   // nudge grid +10 ms
        case EV_BWD:  if (dk.grid_offset >= DK_RATE / 100) dk.grid_offset -= DK_RATE / 100; break;
        case EV_LONG_PRESS: return M_MAIN;
        default: break;
    }
    return 0;
}

// ---- Setup --------------------------------------------------------------------
static const char *setup_labels[] = {"Track", "Sync", "Clock Src", "Clock", "Loop", "BPM", "Analyze"};
#define DK_SETUP_N 7

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
            case 6:
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
            if(pos == 6){ deck_analyze_start(); setup_redraw(pos, sel); break; }
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
