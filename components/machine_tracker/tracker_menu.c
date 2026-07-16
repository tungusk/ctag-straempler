// Tracker UI — Live (title, format, position, transport bar), Setup
// (file/loop/sound/sync/clock), and the module browser. Change-driven redraws
// only (full-region repaints every tick strobe the screen). Cloned from the
// deck's menu patterns.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <esp_http_server.h>
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "clock.h"
#include "beatlisten.h"
#include "menu_config.h"
#include "audio.h"
#include "setup_menu.h"
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
#define BODY_Y   140        // sample-name panel: BELOW the bar (off by default)
static char s_type[48] = "", s_stat[48] = "";
static char s_title[TRK_TITLE_LEN] = "";
static char s_body_sig[32] = "";
static int  s_body_top = -999;
static int  s_last_barx = -1;
static int  s_bar_state = -1;
static int  s_bar_loop  = -1;
static int  s_last_loopa = -1, s_last_loopb = -1;

static const char *state_word(void){
    switch (trk.state){
        case TRK_LOADING: return "LOAD";
        case TRK_READY:   return trk.playing ? (trk.loading ? "BUF" : "PLAY") : "STOP";
        case TRK_FAIL:    return "FAIL";
        default:          return "----";
    }
}

// THE PLAY BAR (Arlo: "beef the whole thing up... make it glorious"). A big
// solid bar: LIGHT green body with the LOOP WINDOW punched into it DARK ("invert
// the greens so the loop is dark on light"). The loop reads as a hole cut in the
// song rather than a bright sticker laid on top — and the white playhead pops
// against both.
//
// Both the window and the playhead are measured in STEPS. They used to disagree
// — the head came from module TIME while the window came from step positions, so
// on any module whose tempo moves, the two axes drift apart and the window never
// sat where the head was (Arlo: "i'm not seeing the loop size").
#define TBAR_X  8
#define TBAR_Y  106        // clear of the status line (the beefy bar ran into it)
#define TBAR_H  26           // beefy
#define TBAR_W  (_width - 16)
#define TBAR_HEAD_W 4
#define LOOP_MIN_W  6        // a 4-step loop in a 1000-step song is ~1px: floor it

// LIGHT = the song (the bar's body), DARK = the loop window cut into it. Green
// while playing, blue when stopped, red on a failed load — same pairing either way.
static color_t bar_body(void){
    if (trk.state == TRK_FAIL) return (color_t){190, 70, 70};
    return trk.playing ? (color_t){70, 215, 115} : (color_t){70, 130, 210};
}
static color_t bar_loop(void){
    if (trk.state == TRK_FAIL) return (color_t){70, 20, 20};
    // RETRIG is not a loop of the song — it is a stutter of the BUFFER, and the
    // song is frozen under it. Give it its own colour so the two never read alike.
    if (trk.retrig_div > 0) return (color_t){120, 20, 70};
    return trk.playing ? (color_t){16, 62, 32} : (color_t){22, 42, 88};
}

// the playhead's position in per-mille of the song — in STEPS, the same axis the
// loop window uses (falls back to time only if the step map is missing)
static int bar_pos_pm(void){
    if (trk.total_steps && trk.n_orders && trk.cur_pos >= 0 &&
        trk.cur_pos < trk.n_orders){
        uint32_t st = trk.order_step0[trk.cur_pos] + (uint32_t)trk.cur_row;
        if (st > trk.total_steps) st = trk.total_steps;
        return (int)((uint64_t)st * 1000 / trk.total_steps);
    }
    if (trk.total_ms > 0) return (int)((int64_t)trk.time_ms * 1000 / trk.total_ms);
    return 0;
}

static int bar_x_of(int pm){ return TBAR_X + 2 + pm * (TBAR_W - 8) / 1000; }

static void loop_px(int *pa, int *pb){
    int a = bar_x_of(trk.loop_a_pm), b = bar_x_of(trk.loop_b_pm);
    if (b < a + LOOP_MIN_W) b = a + LOOP_MIN_W;
    int lim = TBAR_X + TBAR_W - 2;
    if (b > lim) { b = lim; if (a > b - LOOP_MIN_W) a = b - LOOP_MIN_W; }
    *pa = a; *pb = b;
}

// paint the bar across [sx, sx+sw): the body is LIGHT, the loop window is cut
// into it DARK (all light when there is no loop). Doubles as the playhead's erase.
static void bar_paint_bg(int sx, int sw){
    if (sw <= 0) return;
    int iy = TBAR_Y + 1, ih = TBAR_H - 2;          // inside the frame
    if (!trk.loop_engage){
        TFT_fillRect(sx, iy, sw, ih, bar_body());
        return;
    }
    int a, b;
    loop_px(&a, &b);
    int x = sx, end = sx + sw;
    while (x < end){
        int run;
        color_t c;
        if (x < a)      { run = a < end ? a : end; c = bar_body(); }
        else if (x < b) { run = b < end ? b : end; c = bar_loop(); }
        else            { run = end;               c = bar_body(); }
        if (run > x) TFT_fillRect(x, iy, run - x, ih, c);
        x = run;
    }
}

static void draw_bar_frame(void){
    _fg = (color_t){60, 70, 90};
    TFT_drawRect(TBAR_X, TBAR_Y, TBAR_W, TBAR_H, _fg);      // a thin containing frame
    bar_paint_bg(TBAR_X + 1, TBAR_W - 2);
    _bg = TFT_BLACK;
    s_last_barx = -1;
    s_last_loopa = -1; s_last_loopb = -1;
    s_bar_state = trk.playing ? 1 : 0;
    s_bar_loop  = trk.loop_engage ? 1 : 0;
}

static void draw_bar(void){
    int a = -1, b = -1;
    if (trk.loop_engage) loop_px(&a, &b);
    if ((trk.playing ? 1 : 0) != s_bar_state ||
        (trk.loop_engage ? 1 : 0) != s_bar_loop ||
        a != s_last_loopa || b != s_last_loopb){
        draw_bar_frame();
        s_last_loopa = a; s_last_loopb = b;
        s_last_barx = -1;
    }
    int x = bar_x_of(bar_pos_pm());
    if (x == s_last_barx) return;
    if (s_last_barx > 0) bar_paint_bg(s_last_barx, TBAR_HEAD_W);       // erase
    TFT_fillRect(x, TBAR_Y + 1, TBAR_HEAD_W, TBAR_H - 2, (color_t){245, 245, 245});
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

// NUDGE indicator: accumulates detents while the ~800 ms TTL is fresh and
// rides the change-driven status repaint (draw_info compares strings, so the
// prefix appears/expires without any extra draw calls)
static int s_nudge_val = 0;
static uint32_t s_nudge_tick = 0;
static void nudge_note(int dir){
    uint32_t tk = xTaskGetTickCount();
    if (tk - s_nudge_tick >= pdMS_TO_TICKS(800)) s_nudge_val = 0;
    s_nudge_val += dir;
    s_nudge_tick = tk;
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
        // bpm slot: when clock-synced and locked, show the EXTERNAL tempo the
        // module is actually following, tagged EXT — the sync-is-live indicator
        char bs[16];
        if (trk.sync && trk.ci.clk.locked && trk.ci.clk.bpm > 0)
            snprintf(bs, sizeof(bs), "%d bpm EXT",
                     (int)(trk.ci.clk.bpm / TRK_PPB_EFF() + 0.5f));
        else
            snprintf(bs, sizeof(bs), "%d bpm", trk.mod_bpm);
        if (trk.loop_engage && trk.retrig_div > 0)
            snprintf(st, sizeof(st), "RETRIG  1/%d step  @ %02d:%02d  %s",
                     trk.retrig_div, trk.loop_start_ord, trk.loop_start_row, bs);
        else if (trk.loop_engage)
            snprintf(st, sizeof(st), "LOOP  %d step%s  @ %02d:%02d  %s",
                     trk.loop_len, trk.loop_len > 1 ? "s" : "",
                     trk.loop_start_ord, trk.loop_start_row, bs);
        else if (s_nudge_val != 0 &&
                 xTaskGetTickCount() - s_nudge_tick < pdMS_TO_TICKS(800))
            snprintf(st, sizeof(st), "NUDGE %+d   pat %02d/%02d   %s",
                     s_nudge_val, trk.cur_pos, trk.num_pat, bs);
        else if (trk.flt_mode)
            snprintf(st, sizeof(st), "%s  pat %02d/%02d  %s  %s",
                     state_word(), trk.cur_pos, trk.num_pat, bs,
                     trk.flt_mode == 1 ? "LP" : "HP");
        else
            snprintf(st, sizeof(st), "%s   pat %02d/%02d   %s",
                     state_word(), trk.cur_pos, trk.num_pat, bs);
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

    // knob7/CV7 scrolls the list — but only in the TOP HALF of its range (the
    // lower half rests at line 1), and NOT while looping (there CV7 = loop len).
    uint16_t cv[8]; audio_get_cv(cv);
    int top = 0;
    if (trk.loop_engage){
        top = s_body_top >= 0 ? s_body_top : 0;   // hold position while looping
    } else if (maxtop > 0 && cv[6] > 2048){
        top = (int)((int64_t)(cv[6] - 2048) * maxtop / (4095 - 2048));
        if (top < 0) top = 0;
        if (top > maxtop) top = maxtop;
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
    static int  scrub_target = -1;
    static bool scrub_pending = false, scrub_moved = false;
    switch(event){
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_FAST:
        case EV_TIMER_REPEATING_SLOW:
            draw_title();
            draw_info();
            draw_bar();
            draw_body(false);
            // commit a pattern scrub only once the encoder has SETTLED (no new
            // detent for a tick); jumping on every detent mid-scrub is choppy.
            // The engine then applies it on the next beat.
            if (scrub_pending){
                if (scrub_moved) scrub_moved = false;
                else { trk.seek_pos = scrub_target; trk.seek_req = true; scrub_pending = false; }
            }
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
        // MODAL encoder (house pattern, deck precedent): synced+locked =
        // STEP-NUDGE (whole-pulse row shifts the servo can't pull back out);
        // otherwise the existing pattern scrub. Loop mode keeps its behaviour
        // (CV6 covers position there; a nudge would be undone at re-seat).
        case EV_FWD:
            if (trk.sync && trk.ci.clk.locked && !trk.loop_engage && trk.playing){
                trk.nudge_req++;
                nudge_note(+1);
            } else if (trk.num_pat > 0){
                if (!scrub_pending) scrub_target = trk.cur_pos;
                scrub_target = (scrub_target + 1) % trk.num_pat;
                scrub_pending = true; scrub_moved = true;
            }
            break;
        case EV_BWD:
            if (trk.sync && trk.ci.clk.locked && !trk.loop_engage && trk.playing){
                trk.nudge_req--;
                nudge_note(-1);
            } else if (trk.num_pat > 0){
                if (!scrub_pending) scrub_target = trk.cur_pos;
                scrub_target = (scrub_target + trk.num_pat - 1) % trk.num_pat;
                scrub_pending = true; scrub_moved = true;
            }
            break;
        case EV_LONG_PRESS: return M_TRACKER_SETUP;   // toggle Live -> Setup
        default: break;
    }
    return 0;
}

// ---- Setup (shared setup-menu framework) ------------------------------------
// Module = ACTION (opens the module browser); Loop/Sound/Sync/Clock Src/Info
// Text/Loop Freeze = TOGGLE cycles; Clock (ppb ladder) = RANGE (bidirectional
// turn through trk_ppb_names, clamped 0..4).
static const setup_item_t trk_setup_items[] = {
    {"Module",      ST_ACTION},
    {"Loop",        ST_TOGGLE},
    {"Sound",       ST_TOGGLE},
    {"Sync",        ST_TOGGLE},
    {"Clock Src",   ST_TOGGLE},
    {"Clock",       ST_RANGE},
    {"Info Text",   ST_TOGGLE},
    {"Loop Freeze", ST_TOGGLE},
};

static void trk_setup_render(int i, char *v, size_t n){
    switch(i){
        case 0: snprintf(v, n, "%s", trk.file[0] ? trk.file : "(none)"); break;
        case 1: snprintf(v, n, "%s", trk.loop ? "ON" : "OFF"); break;
        case 2: snprintf(v, n, "%s", trk.amiga ? "Amiga" : "Clean"); break;
        case 3: snprintf(v, n, "%s", trk.sync ? "ON" : "OFF"); break;
        case 4: snprintf(v, n, "%s", clock_source_name(trk.clk_src)); break;
        case 5: snprintf(v, n, "%s", trk_ppb_names[trk.ppb_idx]); break;
        case 6: snprintf(v, n, "%s", trk.show_text ? "ON" : "OFF"); break;
        case 7: snprintf(v, n, "%s", trk.loop_freeze ? "ON" : "OFF"); break;
    }
}

static void trk_setup_adj(int i, int dir){
    switch(i){
        case 1: trk.loop = !trk.loop; break;
        case 2: trk.amiga = !trk.amiga; trk.sound_dirty = true; break;
        case 3: trk.sync = !trk.sync; break;
        case 4:
            // CV1..8 + AUDIO (TR1/TR2 are play/loop); AUDIO wakes the ear
            trk.clk_src = clock_source_cycle_cv_audio(trk.clk_src, dir);
            if (trk.clk_src == CLK_SRC_AUDIO && beatlisten_get_mode() == BL_OFF) {
                beatlisten_set_mode(BL_GROOVE);
                configSetIntSetting("blisten", BL_GROOVE);
            }
            break;
        case 5: trk.ppb_idx += dir; if (trk.ppb_idx < 0) trk.ppb_idx = 0; if (trk.ppb_idx > 4) trk.ppb_idx = 4; break;
        case 6: trk.show_text = !trk.show_text; break;
        case 7: trk.loop_freeze = !trk.loop_freeze; break;
    }
}

static int trk_setup_action(int i){
    if (i == 0){ refresh_mods(); s_load_ret = M_TRACKER_SETUP; return M_TRACKER_LOAD; }
    return 0;
}

static setup_menu_t trk_setup = {
    .items = trk_setup_items, .n = 8, .title = "Tracker Setup",
    .aff_label = "Machine", .aff_target = M_MORE, .live_target = M_TRACKER_LIVE,
    .render = trk_setup_render, .adjust = trk_setup_adj, .action = trk_setup_action,
};

static int tracker_setup_handler(int it_id, int event, void *ev_data){
    (void)it_id; (void)ev_data;
    return setup_menu_event(&trk_setup, event);
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
            // pressing the already-loaded track just exits (no reload); a
            // different track loads. Either way we leave the browser.
            if (s_n_mods && strcmp(s_mods[s_mod_idx], trk.file) != 0)
                tracker_request_load(s_mods[s_mod_idx]);
            return s_load_ret;
        case EV_LONG_PRESS: return s_load_ret;   // cancel — exit without loading
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
