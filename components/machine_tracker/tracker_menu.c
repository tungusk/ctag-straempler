// Tracker UI — Live (title, format, position, transport bar), Setup
// (file/loop/sound/sync/clock), and the module browser. Change-driven redraws
// only (full-region repaints every tick strobe the screen). Cloned from the
// deck's menu patterns.
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
#include "tracker_priv.h"

static char (*s_mods)[TRK_NAME_LEN] = NULL;
static int  s_n_mods = 0, s_mod_idx = 0;
static int  s_load_ret = M_TRACKER_SETUP;

static void refresh_mods(void){
    s_n_mods = tracker_list_modules(&s_mods);
    s_mod_idx = 0;
    for (int i = 0; i < s_n_mods; i++)
        if (strcmp(s_mods[i], trk.file) == 0) { s_mod_idx = i; break; }
}

// ---- Live -------------------------------------------------------------------
static char s_info1[64] = "", s_info2[64] = "";
static int  s_last_barx = -1;
static int  s_bar_state = -1;

static const char *state_word(void){
    switch (trk.state){
        case TRK_LOADING: return "LOAD";
        case TRK_READY:   return trk.playing ? (trk.loading ? "BUF" : "PLAY") : "STOP";
        case TRK_FAIL:    return "FAIL";
        default:          return "----";
    }
}

// transport bar: full width, green playing / blue stopped background, white
// position marker. _bg is a shared global — restore it after (hint line was
// blue-on-blue in the deck until this was fixed).
#define TBAR_X 8
#define TBAR_Y 116
#define TBAR_H 28
#define TBAR_W (_width - 16)

static color_t tbar_bg(void){
    if (trk.state == TRK_FAIL) return (color_t){120, 40, 40};
    return trk.playing ? (color_t){25, 120, 50} : (color_t){30, 60, 140};
}

static void draw_bar_frame(void){
    TFT_fillRect(TBAR_X, TBAR_Y, TBAR_W, TBAR_H, tbar_bg());
    _fg = (color_t){70, 70, 90};
    TFT_drawRect(TBAR_X, TBAR_Y, TBAR_W, TBAR_H, _fg);
    _bg = TFT_BLACK;
    s_last_barx = -1;
    s_bar_state = trk.playing ? 1 : 0;
}

static void draw_bar(void){
    if ((trk.playing ? 1 : 0) != s_bar_state) draw_bar_frame();
    if (trk.total_ms <= 0) return;
    int frac = (int)((int64_t)trk.time_ms * (TBAR_W - 8) / trk.total_ms);
    int x = TBAR_X + 2 + frac;
    if (x == s_last_barx) return;
    if (s_last_barx > 0)
        TFT_fillRect(s_last_barx, TBAR_Y + 1, 4, TBAR_H - 2, tbar_bg());
    TFT_fillRect(x, TBAR_Y + 1, 4, TBAR_H - 2, (color_t){235, 235, 235});
    s_last_barx = x;
}

static void draw_info(void){
    int fh = TFT_getfontheight();
    int y = fh + 44;
    char s1[64], s2[64];
    if (trk.state == TRK_FAIL)
        snprintf(s1, sizeof(s1), "FAIL: %.20s", trk.fail_why);
    else
        snprintf(s1, sizeof(s1), "%.10s %dch  %s  P%02d/%02d",
                 trk.fmt, trk.channels, state_word(), trk.cur_pos, trk.num_pat);
    if (trk.sync){
        if (trk.clk.locked) snprintf(s2, sizeof(s2), "sync %d bpm  LOCK", trk.cur_bpm);
        else snprintf(s2, sizeof(s2), "sync: waiting CV%d", trk.clk_src + 1);
    } else s2[0] = 0;
    if (strcmp(s1, s_info1) != 0){
        strcpy(s_info1, s1);
        _bg = TFT_BLACK; TFT_fillRect(0, y, _width, fh + 4, _bg);
        _fg = (trk.state == TRK_FAIL) ? (color_t){230, 120, 120} : TFT_WHITE;
        TFT_print(s1, 8, y);
    }
    if (strcmp(s2, s_info2) != 0){
        strcpy(s_info2, s2);
        _bg = TFT_BLACK; TFT_fillRect(0, y + fh + 6, _width, fh + 4, _bg);
        _fg = trk.clk.locked ? (color_t){40, 200, 90} : TFT_LIGHTGREY;
        if (s2[0]) TFT_print(s2, 8, y + fh + 6);
    }
}

static char s_last_title[TRK_TITLE_LEN] = "";
static void draw_title(void){
    if (strcmp(trk.title, s_last_title) == 0) return;
    strcpy(s_last_title, trk.title);
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    int bh = TFT_getfontheight();
    _bg = TFT_BLACK; TFT_fillRect(0, TFT_getfontheight() + 2, _width, bh + 4, _bg);
    _fg = TFT_WHITE;
    char nm[22];
    snprintf(nm, sizeof(nm), "%.20s", trk.title[0] ? trk.title : "(none)");
    TFT_print(nm, 8, TFT_getfontheight() + 4);
    cfont = f;
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Tracker", 6, 4);
    s_last_title[0] = 0; draw_title();
    s_info1[0] = 0; s_info2[0] = 0; draw_info();
    s_bar_state = -1; draw_bar();
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn:seek press:load TR1:restart TR2:stop", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int tracker_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_FAST:
        case EV_TIMER_REPEATING_SLOW:
            draw_title();
            draw_info();
            draw_bar();
            if (event == EV_TIMER_REPEATING_SLOW){
                char dbg[48];
                snprintf(dbg, sizeof(dbg), "%c %.6s %dch r%d S%lu",
                         trk.playing ? 'P' : 's', trk.fmt, trk.channels,
                         trk.cur_row, (unsigned long)trk.dbg_starve);
                audio_status_set_voices("tracker", dbg);
            }
            break;
        case EV_SHORT_PRESS:
            refresh_mods(); s_load_ret = M_TRACKER_LIVE;
            return M_TRACKER_LOAD;
        case EV_FWD: if (trk.num_pat > 0){ trk.seek_pos = (trk.cur_pos + 1) % trk.num_pat; trk.seek_req = true; } break;
        case EV_BWD: if (trk.num_pat > 0){ trk.seek_pos = (trk.cur_pos + trk.num_pat - 1) % trk.num_pat; trk.seek_req = true; } break;
        case EV_LONG_PRESS: return M_MAIN;
        default: break;
    }
    return 0;
}

// ---- Setup ------------------------------------------------------------------
static const char *setup_labels[] = {"Module", "Loop", "Sound", "Sync", "Clock Src", "Clock"};
#define TRK_SETUP_N 6

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE;
    TFT_print("Tracker Setup", 6, 4);
    for (int i = 0; i < TRK_SETUP_N; i++){
        int y = fh + 16 + i * (fh + 8);
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
        TFT_print((char*)setup_labels[i], 8, y);
        char v[28];
        switch(i){
            case 0: snprintf(v, sizeof(v), "%s", trk.file[0] ? trk.file : "(none)"); break;
            case 1: snprintf(v, sizeof(v), "%s", trk.loop ? "ON" : "OFF"); break;
            case 2: snprintf(v, sizeof(v), "%s", trk.amiga ? "Amiga" : "Clean"); break;
            case 3: snprintf(v, sizeof(v), "%s", trk.sync ? "ON" : "OFF"); break;
            case 4: snprintf(v, sizeof(v), "CV%d", trk.clk_src + 1); break;
            case 5: snprintf(v, sizeof(v), "%s", trk_ppb_names[trk.ppb_idx]); break;
        }
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("libxmp — plays MOD/XM/IT/S3M/669 and more", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void setup_adj(int i, int dir){
    switch(i){
        case 1: trk.loop = !trk.loop; break;
        case 2: trk.amiga = !trk.amiga; trk.sound_dirty = true; break;
        case 3: trk.sync = !trk.sync; break;
        case 4: trk.clk_src = (trk.clk_src + (dir > 0 ? 1 : 7)) & 7; break;
        case 5: trk.ppb_idx += dir; if (trk.ppb_idx < 0) trk.ppb_idx = 0; if (trk.ppb_idx > 4) trk.ppb_idx = 4; break;
    }
}

static int tracker_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; setup_redraw(pos, sel); break;
        case EV_FWD:
            if (sel) setup_adj(pos, +1); else pos = (pos + 1) % TRK_SETUP_N;
            setup_redraw(pos, sel); break;
        case EV_BWD:
            if (sel) setup_adj(pos, -1); else pos = (pos + TRK_SETUP_N - 1) % TRK_SETUP_N;
            setup_redraw(pos, sel); break;
        case EV_SHORT_PRESS:
            if (pos == 0){ refresh_mods(); s_load_ret = M_TRACKER_SETUP; return M_TRACKER_LOAD; }
            sel = !sel; setup_redraw(pos, sel); break;
        case EV_LONG_PRESS: return M_MAIN;
        default: break;
    }
    return 0;
}

// ---- Load browser -----------------------------------------------------------
static void load_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; _fg = TFT_LIGHTGREY;
    char h[48];
    snprintf(h, sizeof(h), "Load Module  (%d/%d)", s_n_mods ? s_mod_idx + 1 : 0, s_n_mods);
    TFT_print(h, _width / 2 - TFT_getStringWidth(h) / 2, 4);
    if (!s_n_mods){
        char *m = "no modules in usr/";
        TFT_print(m, _width / 2 - TFT_getStringWidth(m) / 2, _height / 2);
        return;
    }
    int cy = _height / 2;
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    int bigfh = TFT_getfontheight();
    _fg = TFT_WHITE;
    char *sel = s_mods[s_mod_idx];
    TFT_print(sel, _width / 2 - TFT_getStringWidth(sel) / 2, cy - bigfh / 2);
    cfont = f;
    _fg = (color_t){110, 110, 110};
    for (int k = 1; k <= 3; k++){
        int up = s_mod_idx - k, dn = s_mod_idx + k;
        int yup = cy - bigfh / 2 - k * (fh + 4) - 4;
        int ydn = cy + bigfh / 2 + (k - 1) * (fh + 4) + 6;
        if (up >= 0){ char *nn = s_mods[up]; TFT_print(nn, _width / 2 - TFT_getStringWidth(nn) / 2, yup); }
        if (dn < s_n_mods){ char *nn = s_mods[dn]; TFT_print(nn, _width / 2 - TFT_getStringWidth(nn) / 2, ydn); }
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    char *hint = "turn:browse  press:load  hold:cancel";
    TFT_print(hint, _width / 2 - TFT_getStringWidth(hint) / 2, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int tracker_load_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: load_redraw(); break;
        case EV_FWD: if (s_n_mods){ s_mod_idx = (s_mod_idx + 1) % s_n_mods; load_redraw(); } break;
        case EV_BWD: if (s_n_mods){ s_mod_idx = (s_mod_idx + s_n_mods - 1) % s_n_mods; load_redraw(); } break;
        case EV_SHORT_PRESS:
            if (s_n_mods) tracker_request_load(s_mods[s_mod_idx]);
            return s_load_ret;
        case EV_LONG_PRESS: return s_load_ret;
        default: break;
    }
    return 0;
}

// ---- registration -----------------------------------------------------------
static void tracker_register_pages(void *menusys){
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_TRACKER_LIVE);  menusys_item_set_default_cb(_ms, M_TRACKER_LIVE, tracker_live_handler);
    menusys_new_item(_ms, M_TRACKER_SETUP); menusys_item_set_default_cb(_ms, M_TRACKER_SETUP, tracker_setup_handler);
    menusys_new_item(_ms, M_TRACKER_LOAD);  menusys_item_set_default_cb(_ms, M_TRACKER_LOAD, tracker_load_handler);
}

static const char *const tracker_main_items[] = {"Live", "Setup"};
static const int tracker_main_targets[] = {M_TRACKER_LIVE, M_TRACKER_SETUP};

const machine_ui_t tracker_menu_ui = {
    .main_items = tracker_main_items,
    .main_targets = tracker_main_targets,
    .n_main = 2,
    .register_pages = tracker_register_pages,
    .boot_target = M_TRACKER_LIVE,
};
