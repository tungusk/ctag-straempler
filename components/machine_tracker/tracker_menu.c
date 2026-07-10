// Tracker UI — Live (title, format, position, transport bar), Setup
// (file/loop/sound/sync/clock), and the module browser. Change-driven redraws
// only (full-region repaints every tick strobe the screen). Cloned from the
// deck's menu patterns.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <esp_http_server.h>
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
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
// Live layout (320x240, absolute coords — pages resetclipwin before drawing):
// header, big module title, full module type, status, slim transport bar, then
// the module "message" panel — the sample/instrument name slots in a small
// font, scrolled by knob7/CV7 (toggled by trk.show_text; off => blank).
#define HDR_Y    4
#define TITLE_Y  22
#define TYPE_Y   58
#define STAT_Y   80
#define BODY_Y   118
static char s_type[48] = "", s_stat[48] = "";
static char s_title[TRK_TITLE_LEN] = "";
static char s_body_sig[32] = "";
static int  s_body_top = -999;
static int  s_last_barx = -1;
static int  s_bar_state = -1;
static int  s_bar_loop  = -1;

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
#define TBAR_Y 100
#define TBAR_H 12
#define TBAR_W (_width - 16)

static color_t tbar_bg(void){
    if (trk.state == TRK_FAIL) return (color_t){120, 40, 40};
    if (trk.loop_engage)       return (color_t){150, 45, 95};   // pink = loop mode
    return trk.playing ? (color_t){25, 120, 50} : (color_t){30, 60, 140};
}

static void draw_bar_frame(void){
    TFT_fillRect(TBAR_X, TBAR_Y, TBAR_W, TBAR_H, tbar_bg());
    _fg = (color_t){70, 70, 90};
    TFT_drawRect(TBAR_X, TBAR_Y, TBAR_W, TBAR_H, _fg);
    _bg = TFT_BLACK;
    s_last_barx = -1;
    s_bar_state = trk.playing ? 1 : 0;
    s_bar_loop  = trk.loop_engage ? 1 : 0;
}

static void draw_bar(void){
    if ((trk.playing ? 1 : 0) != s_bar_state ||
        (trk.loop_engage ? 1 : 0) != s_bar_loop) draw_bar_frame();
    if (trk.total_ms <= 0) return;
    int frac = (int)((int64_t)trk.time_ms * (TBAR_W - 8) / trk.total_ms);
    int x = TBAR_X + 2 + frac;
    if (x == s_last_barx) return;
    if (s_last_barx > 0)
        TFT_fillRect(s_last_barx, TBAR_Y + 1, 4, TBAR_H - 2, tbar_bg());
    TFT_fillRect(x, TBAR_Y + 1, 4, TBAR_H - 2, (color_t){235, 235, 235});
    s_last_barx = x;
}

static void draw_title(void){
    if (strcmp(trk.title, s_title) == 0) return;
    strlcpy(s_title, trk.title, sizeof(s_title));
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    int bh = TFT_getfontheight();
    _bg = TFT_BLACK; TFT_fillRect(0, TITLE_Y, _width, bh + 4, _bg);
    _fg = TFT_WHITE;
    char nm[24]; snprintf(nm, sizeof(nm), "%.22s", trk.title[0] ? trk.title : "(untitled)");
    TFT_print(nm, 8, TITLE_Y);
    cfont = f;
}

static void draw_info(void){
    TFT_setFont(DEFAULT_FONT, NULL);
    int fh = TFT_getfontheight();
    char ty[48], st[48];
    if (trk.state == TRK_FAIL){
        snprintf(ty, sizeof(ty), "FAIL: %.36s", trk.fail_why);
        st[0] = 0;
    } else {
        // full module type on its own line (no longer truncated to 10 chars)
        snprintf(ty, sizeof(ty), "%s   %d ch", trk.fmt[0] ? trk.fmt : "(unknown)", trk.channels);
        if (trk.loop_engage)
            snprintf(st, sizeof(st), "LOOP  %d step%s  @ %02d:%02d",
                     trk.loop_len, trk.loop_len > 1 ? "s" : "",
                     trk.loop_start_ord, trk.loop_start_row);
        else
            snprintf(st, sizeof(st), "%s   pat %02d/%02d   %d bpm",
                     state_word(), trk.cur_pos, trk.num_pat, trk.mod_bpm);
    }
    if (strcmp(ty, s_type) != 0){
        strcpy(s_type, ty);
        _bg = TFT_BLACK; TFT_fillRect(0, TYPE_Y, _width, fh + 4, _bg);
        _fg = (trk.state == TRK_FAIL) ? (color_t){230, 120, 120} : (color_t){120, 200, 255};
        TFT_print(ty, 8, TYPE_Y);
    }
    if (strcmp(st, s_stat) != 0){
        strcpy(s_stat, st);
        _bg = TFT_BLACK; TFT_fillRect(0, STAT_Y, _width, fh + 4, _bg);
        _fg = trk.loop_engage ? (color_t){235, 120, 175} : TFT_WHITE;
        if (st[0]) TFT_print(st, 8, STAT_Y);
    }
}

// Module "message" panel: the sample/instrument name slots in a small font,
// scrolled by knob7/CV7. When trk.show_text is off, the panel stays blank.
// Redraws only when scroll position or content changes.
static void draw_body(bool force){
    Font f = cfont;
    if (!trk.show_text){
        if (!force && s_body_top == -1){ cfont = f; return; }   // already blank
        s_body_top = -1; s_body_sig[0] = 0;
        _bg = TFT_BLACK; TFT_fillRect(0, BODY_Y, _width, _height - BODY_Y, _bg);
        cfont = f;
        return;
    }
    TFT_setFont(DEF_SMALL_FONT, NULL);
    int lh = TFT_getfontheight() + 2;
    int rows = (_height - BODY_Y) / lh; if (rows < 1) rows = 1;
    int n = trk.n_names;
    int maxtop = n > rows ? n - rows : 0;

    // knob7/CV7 scrolls the list — but NOT while looping (there CV7 = loop
    // length), and a freshly loaded module starts at line 1 until the user
    // actually grabs the knob (so the initial view isn't wherever CV7 sits).
    static int  scroll_latch = 0;
    static int  cv7_base = -1;
    static char content_sig[24] = "";
    uint16_t cv[8]; audio_get_cv(cv);
    char csig[24]; snprintf(csig, sizeof(csig), "%d:%.16s", n, n ? trk.names[0] : "");
    if (strcmp(csig, content_sig) != 0){          // new module → back to line 1
        strlcpy(content_sig, csig, sizeof(content_sig));
        scroll_latch = 0;
        cv7_base = cv[6];
    }
    int top = 0;
    if (trk.loop_engage){
        top = s_body_top >= 0 ? s_body_top : 0;   // hold position while looping
    } else if (maxtop > 0){
        int d = (int)cv[6] - cv7_base; if (d < 0) d = -d;
        if (!scroll_latch && d > 150) scroll_latch = 1;
        if (scroll_latch){
            top = (int)((int64_t)cv[6] * maxtop / 4095);
            if (top < 0) top = 0;
            if (top > maxtop) top = maxtop;
        }
    }
    char sig[32]; snprintf(sig, sizeof(sig), "N%d.%d:%.10s", n, top, n ? trk.names[0] : "");
    if (!force && top == s_body_top && strcmp(sig, s_body_sig) == 0){ cfont = f; return; }
    s_body_top = top; strlcpy(s_body_sig, sig, sizeof(s_body_sig));
    _bg = TFT_BLACK; TFT_fillRect(0, BODY_Y, _width, _height - BODY_Y, _bg);
    int y = BODY_Y;
    for (int i = 0; i < rows && top + i < n; i++){
        int has = trk.names[top + i][0] != 0;
        _fg = has ? (color_t){205, 205, 205} : (color_t){60, 60, 60};
        char ln[40]; snprintf(ln, sizeof(ln), "%2d %.30s", top + i + 1,
                              has ? trk.names[top + i] : "-");
        TFT_print(ln, 8, y); y += lh;
    }
    cfont = f;
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_setFont(DEFAULT_FONT, NULL);
    TFT_print("Tracker", 6, HDR_Y);
    s_title[0] = 0; draw_title();
    s_type[0] = 0; s_stat[0] = 0; draw_info();
    s_bar_state = -1; draw_bar();
    s_body_sig[0] = 0; s_body_top = -999; draw_body(true);
}

static int tracker_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_FAST:
        case EV_TIMER_REPEATING_SLOW:
            draw_title();
            draw_info();
            draw_bar();
            draw_body(false);
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
        case EV_LONG_PRESS: return M_TRACKER_SETUP;   // toggle Live -> Setup
        default: break;
    }
    return 0;
}

// ---- Setup ------------------------------------------------------------------
static const char *setup_labels[] = {"Module", "Loop", "Sound", "Sync", "Clock Src", "Clock", "Info Text", "Loop Freeze"};
#define TRK_SETUP_N 8

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE;
    TFT_print("Tracker Setup", 6, 4);
    menuTFTPrintAffordance("System", pos == -1);
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
            case 6: snprintf(v, sizeof(v), "%s", trk.show_text ? "ON" : "OFF"); break;
            case 7: snprintf(v, sizeof(v), "%s", trk.loop_freeze ? "ON" : "OFF"); break;
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
        case 6: trk.show_text = !trk.show_text; break;
        case 7: trk.loop_freeze = !trk.loop_freeze; break;
    }
}

static int tracker_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; setup_redraw(pos, sel); break;
        case EV_FWD:
            if (sel) setup_adj(pos, +1); else { pos++; if (pos >= TRK_SETUP_N) pos = -1; }
            setup_redraw(pos, sel); break;
        case EV_BWD:
            if (sel) setup_adj(pos, -1); else { pos--; if (pos < -1) pos = TRK_SETUP_N - 1; }
            setup_redraw(pos, sel); break;
        case EV_SHORT_PRESS:
            if (pos == -1) return M_MORE;   // System affordance
            if (pos == 0){ refresh_mods(); s_load_ret = M_TRACKER_SETUP; return M_TRACKER_LOAD; }
            sel = !sel; setup_redraw(pos, sel); break;
        case EV_LONG_PRESS: return M_TRACKER_LIVE;   // toggle Setup -> Live
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

// module upload/list/download/delete now live in the core REST layer
// (rest-api.c, /trk/*) so they work regardless of the active machine.
const machine_ui_t tracker_menu_ui = {
    .main_items = tracker_main_items,
    .main_targets = tracker_main_targets,
    .n_main = 2,
    .register_pages = tracker_register_pages,
    .boot_target = M_TRACKER_LIVE,
};
