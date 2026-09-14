// Freesound panel UI. Until 2026-09-12 this was a single read-only status page
// that told you to go and open a browser; the machine was unusable in a rack.
// Now:
//   M_FS_LIVE     the QUERY LIST — recents, saved, "+ new search..."
//   M_FS_ENTRY    encoder text entry (shared widget) for a new query
//   M_FS_RESULTS  the parsed result list; press fetches into the pool
//   M_FS_SETUP    shared setup rows (and therefore the web Remote mirror)
//
// Typing is deliberately the exception: the query list carries the repeat case,
// which is what makes an encoder-driven search bearable.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <esp_http_server.h>
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "setup_menu.h"
#include "text_entry.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "wifi.h"
#include "fs_auth.h"
#include "fs_priv.h"

extern const httpd_uri_t fs_web_uris[];

static const color_t ACCENT   = {230, 90, 40};
static const color_t DIM      = {110, 110, 120};
static const color_t OKGREEN  = {40, 160, 90};
static const color_t ERRRED   = {220, 60, 60};
static const color_t SELBG    = {10, 18, 56};

// stage-3 redraw discipline: one fillScreen per full repaint, and every element
// skips its own clear while that is in flight
static bool s_skip_clear = false;
#define CLEAR_RECT(x, y, w, h) do { if (!s_skip_clear) TFT_fillRect((x), (y), (w), (h), TFT_BLACK); } while (0)

// ---- shared list geometry ---------------------------------------------------
static int row_h(void)   { return TFT_getfontheight() + 5; }
static int list_top(void){ return TFT_getfontheight() * 3 + 26; }
static int list_rows(void)
{
    int n = (_height - list_top() - TFT_getfontheight() - 4) / row_h();
    return n < 3 ? 3 : n;
}

// draw one row of a list at slot `slot` (0-based, on screen)
static void list_row(int slot, const char *left, const char *right, bool sel, color_t fg)
{
    int y = list_top() + slot * row_h();
    TFT_fillRect(0, y - 2, _width, row_h(), sel ? SELBG : TFT_BLACK);
    _bg = sel ? SELBG : TFT_BLACK;
    _fg = sel ? TFT_CYAN : fg;
    char buf[48];
    strlcpy(buf, left ? left : "", sizeof(buf));
    // trim the label until the right-hand column has room
    int rw = (right && right[0]) ? TFT_getStringWidth((char *)right) + 10 : 4;
    while (buf[0] && TFT_getStringWidth(buf) > _width - 12 - rw)
        buf[strlen(buf) - 1] = 0;
    TFT_print(buf, 6, y);
    if (right && right[0]) {
        _fg = sel ? TFT_CYAN : DIM;
        TFT_print((char *)right, _width - 6 - TFT_getStringWidth((char *)right), y);
    }
}

static void scroll_window(int sel, int n, int *first)
{
    int rows = list_rows();
    if (n <= rows) { *first = 0; return; }
    if (sel < *first) *first = sel;
    if (sel >= *first + rows) *first = sel - rows + 1;
    if (*first > n - rows) *first = n - rows;
    if (*first < 0) *first = 0;
}

static void hint(const char *s)
{
    _bg = TFT_BLACK;
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    CLEAR_RECT(0, _height - TFT_getfontheight() - 2, _width, TFT_getfontheight() + 2);
    TFT_print((char *)s, 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void dur_str(uint16_t ds, char *out, size_t n)
{
    int s = ds / 10;
    if (s >= 60) snprintf(out, n, "%d:%02d", s / 60, s % 60);
    else         snprintf(out, n, "%d.%ds", s, ds % 10);
}

// ============================================================================
// M_FS_LIVE — the query list
// ============================================================================
// row order: recents, then saved, then the "new search" row last
static int live_n(void)      { return fsm.n_recents + fsm.n_saved + 1; }
static bool live_is_new(int i){ return i >= fsm.n_recents + fsm.n_saved; }
static const char *live_query(int i)
{
    if (i < fsm.n_recents) return fsm.recents[i];
    i -= fsm.n_recents;
    if (i < fsm.n_saved)   return fsm.saved[i];
    return NULL;
}

static int s_live_sel = 0, s_live_first = 0;
static int s_hdr_sig = -1, s_stat_sig = -1;

static int hdr_sig(void)  { return (fs_auth_ok() ? 1 : 0) | (isWiFiConnected() ? 2 : 0); }
static int stat_sig(void) { return fsm.phase * 1000 + fsm.progress; }

static void header_block(void)
{
    int fh = TFT_getfontheight();
    CLEAR_RECT(0, fh + 12, _width, fh + 6);
    _bg = TFT_BLACK;
    char ip[20] = {0};
    wifiGetIPString(ip, sizeof(ip));
    _fg = fs_auth_ok() ? OKGREEN : ERRRED;
    TFT_print(fs_auth_ok() ? "key ok" : "NO API KEY", 6, fh + 14);
    bool net = isWiFiConnected() != 0;
    _fg = net ? DIM : ERRRED;
    char s[40];
    snprintf(s, sizeof(s), "%s", net ? ip : "no wifi");
    TFT_print(s, _width - 6 - TFT_getStringWidth(s), fh + 14);
    s_hdr_sig = hdr_sig();
}

// one line: whatever the download pipeline is doing, or the last error
static void status_block(void)
{
    int fh = TFT_getfontheight();
    int y = fh * 2 + 20;
    CLEAR_RECT(0, y - 2, _width, fh + 6);
    _bg = TFT_BLACK;
    char s[80];
    if (fsm.phase == FS_ERROR) {
        _fg = ERRRED;
        snprintf(s, sizeof(s), "error: %s", fsm.err);
    } else if (fsm.phase == FS_IDLE) {
        _fg = DIM;
        snprintf(s, sizeof(s), "%d saved, %d recent", fsm.n_saved, fsm.n_recents);
    } else if (fsm.phase == FS_DONE) {
        _fg = OKGREEN;
        snprintf(s, sizeof(s), "got %s", fsm.cur_name);
    } else {
        _fg = ACCENT;
        snprintf(s, sizeof(s), "%s %s %d%%", fsm.cur_name, fs_phase_name(fsm.phase), fsm.progress);
    }
    TFT_print(s, 6, y);
    s_stat_sig = stat_sig();
}

static void live_list(void)
{
    int n = live_n();
    scroll_window(s_live_sel, n, &s_live_first);
    int rows = list_rows();
    CLEAR_RECT(0, list_top() - 2, _width, rows * row_h() + 4);
    for (int r = 0; r < rows; r++) {
        int i = s_live_first + r;
        if (i >= n) { TFT_fillRect(0, list_top() + r * row_h() - 2, _width, row_h(), TFT_BLACK); continue; }
        if (live_is_new(i)) {
            list_row(r, "+ new search...", "", i == s_live_sel, ACCENT);
        } else {
            bool saved = i >= fsm.n_recents;
            list_row(r, live_query(i), saved ? "saved" : "", i == s_live_sel,
                     saved ? OKGREEN : TFT_LIGHTGREY);
        }
    }
}

static void live_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    s_skip_clear = true;
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Freesound", 6, 4);
    header_block();
    status_block();
    live_list();
    hint("turn:pick  press:search  hold:setup");
    s_skip_clear = false;
}

static int fs_live_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    int n = live_n();
    switch (event) {
        case EV_ENTERED_MENU:
            if (s_live_sel >= n) s_live_sel = n - 1;
            live_full_redraw();
            break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST:
            // per-element signatures: a download in flight must not repaint the list
            if (hdr_sig() != s_hdr_sig)   header_block();
            if (stat_sig() != s_stat_sig) status_block();
            break;
        case EV_FWD:
            if (n > 0) { s_live_sel = (s_live_sel + 1) % n; live_list(); }
            break;
        case EV_BWD:
            if (n > 0) { s_live_sel = (s_live_sel + n - 1) % n; live_list(); }
            break;
        case EV_SHORT_PRESS: {
            if (live_is_new(s_live_sel)) return M_FS_ENTRY;
            const char *q = live_query(s_live_sel);
            if (q && fs_search_start(q, 1) == 0) return M_FS_RESULTS;
            break;
        }
        case EV_LONG_PRESS: return M_FS_SETUP;
        default: break;
    }
    return 0;
}

// ============================================================================
// M_FS_ENTRY — text entry for a new query
// ============================================================================
static int fs_entry_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) {
        TFT_resetclipwin();
        TFT_fillScreen(TFT_BLACK);
        _bg = TFT_BLACK; _fg = TFT_WHITE;
        TFT_print("Freesound", 6, 4);
        // seed with the last query: refining "tabla" to "tabla loop" is the
        // common case, and retyping it on an encoder is the thing to avoid
        text_entry_enter("Search:", fsm.last_query, FS_QUERY_LEN - 1);
        return 0;
    }
    int r = text_entry_event(event);
    if (r == 2) return M_FS_LIVE;
    if (r == 1) {
        if (fs_search_start(text_entry_result(), 1) == 0) return M_FS_RESULTS;
        return M_FS_LIVE;                       // busy — the status line says why
    }
    return 0;
}

// ============================================================================
// M_FS_RESULTS — the parsed result list
// ============================================================================
static int s_res_sel = 0, s_res_first = 0;
static int s_res_sig = -1;
static int s_res_phase_seen = -1;

// A fetch is async, so "hear it when it lands" belongs on the timer tick, not
// on the press that started it.
static void autoplay_poll(void)
{
    if (fsm.phase == s_res_phase_seen) return;
    int was = s_res_phase_seen;
    s_res_phase_seen = fsm.phase;
    if (fsm.phase == FS_DONE && was != FS_DONE && fsm.autoplay && fsm.cur_name[0])
        fs_audition(fsm.cur_name);
}

// search phase + download phase together: any of them changing repaints
static int res_sig(void)
{
    return fsm.search_state * 1000000 + fsm.n_results * 10000 + fsm.phase * 1000
         + (fsm.progress / 4) * 2 + (fs_auditioning() ? 1 : 0);
}

static void res_body(void)
{
    int fh = TFT_getfontheight();
    int rows = list_rows();

    CLEAR_RECT(0, fh + 12, _width, fh + 6);
    _bg = TFT_BLACK;
    char h[64];
    if (fsm.search_state == FS_SEARCH_RUNNING) { _fg = ACCENT;  snprintf(h, sizeof(h), "searching..."); }
    else if (fsm.search_state == FS_SEARCH_ERR){ _fg = ERRRED;  snprintf(h, sizeof(h), "%s", fs_search_err()); }
    else { _fg = DIM; snprintf(h, sizeof(h), "\"%s\"  p%d  %d hits", fsm.last_query, fsm.page, fsm.total); }
    while (h[0] && TFT_getStringWidth(h) > _width - 12) h[strlen(h) - 1] = 0;
    TFT_print(h, 6, fh + 14);

    // second line follows the fetch, so you can watch a download without leaving
    CLEAR_RECT(0, fh * 2 + 18, _width, fh + 6);
    if (fsm.phase != FS_IDLE) {
        char s[80];
        if (fsm.phase == FS_ERROR)      { _fg = ERRRED;  snprintf(s, sizeof(s), "error: %s", fsm.err); }
        else if (fsm.phase == FS_DONE)  { _fg = OKGREEN; snprintf(s, sizeof(s), "got %s", fsm.cur_name); }
        else                            { _fg = ACCENT;  snprintf(s, sizeof(s), "%s %s %d%%", fsm.cur_name, fs_phase_name(fsm.phase), fsm.progress); }
        while (s[0] && TFT_getStringWidth(s) > _width - 12) s[strlen(s) - 1] = 0;
        TFT_print(s, 6, fh * 2 + 20);
    }

    int n = fsm.n_results;
    scroll_window(s_res_sel, n, &s_res_first);
    CLEAR_RECT(0, list_top() - 2, _width, rows * row_h() + 4);
    if (n == 0) {
        _bg = TFT_BLACK; _fg = DIM;
        const char *m = (fsm.search_state == FS_SEARCH_RUNNING) ? "" : "no results";
        TFT_print((char *)m, 6, list_top());
    }
    for (int r = 0; r < rows; r++) {
        int i = s_res_first + r;
        if (i >= n) { TFT_fillRect(0, list_top() + r * row_h() - 2, _width, row_h(), TFT_BLACK); continue; }
        char d[16];
        dur_str(fsm.results[i].dur_ds, d, sizeof(d));
        list_row(r, fsm.results[i].name, d, i == s_res_sel, TFT_LIGHTGREY);
    }
    s_res_sig = res_sig();
}

static void res_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    s_skip_clear = true;
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Results", 6, 4);
    res_body();
    hint("turn:browse  press:get/hear  hold:back");
    s_skip_clear = false;
}

static int fs_results_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    int n = fsm.n_results;
    switch (event) {
        case EV_ENTERED_MENU:
            s_res_sel = 0; s_res_first = 0;
            s_res_phase_seen = fsm.phase;
            res_full_redraw();
            break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST:
            autoplay_poll();
            if (res_sig() != s_res_sig) res_body();
            break;
        case EV_FWD:
            if (n == 0) break;
            if (fs_auditioning()) fs_audition_stop();   // moving on = stop listening
            // running off the end pages forward — there is no room for a pager row
            if (s_res_sel == n - 1 && fsm.page * FS_RESULTS_MAX < fsm.total) {
                if (fs_search_start(fsm.last_query, fsm.page + 1) == 0) { s_res_sel = 0; s_res_first = 0; }
            } else if (s_res_sel < n - 1) s_res_sel++;
            res_body();
            break;
        case EV_BWD:
            if (n == 0) break;
            if (fs_auditioning()) fs_audition_stop();
            if (s_res_sel == 0 && fsm.page > 1) {
                if (fs_search_start(fsm.last_query, fsm.page - 1) == 0) { s_res_sel = 0; s_res_first = 0; }
            } else if (s_res_sel > 0) s_res_sel--;
            res_body();
            break;
        case EV_SHORT_PRESS: {
            if (n == 0 || s_res_sel >= n) break;
            fs_result_t *r = &fsm.results[s_res_sel];
            char name[24];
            fs_safe_name(r->name, r->id, name, sizeof(name));
            // Radio's grammar: press on what is already sounding stops it,
            // press on anything else starts it
            if (fs_auditioning() && strcmp(fs_audition_name(), name) == 0) fs_audition_stop();
            else if (fs_audition(name) != 0)                               // not in the pool yet
                fs_get_start(r->id, name);   // busy/err both land in the status line
            res_body();
            break;
        }
        case EV_LONG_PRESS:
            if (fs_auditioning()) fs_audition_stop();
            return M_FS_LIVE;
        default: break;
    }
    return 0;
}

// ============================================================================
// M_FS_SETUP — shared rows (mirrors to the web Remote tab for free)
// ============================================================================
enum { FR_NEW = 0, FR_SAVE, FR_CLEAR, FR_AUTO, FR_DROP, FR_N };

static const setup_item_t fs_setup_items[] = {
    {"New Search",    ST_ACTION},
    {"Save Query",    ST_ACTION},
    {"Clear Recents", ST_ACTION},
    {"Auto-play",     ST_TOGGLE},
    {"Drop Last",     ST_ACTION},
};
_Static_assert(sizeof(fs_setup_items) / sizeof(fs_setup_items[0]) == FR_N,
               "fs_setup_items and FR_* disagree — a stale count drops the tail rows");

static void fs_setup_val(int i, char *v, size_t n)
{
    switch (i) {
        case FR_NEW:   snprintf(v, n, "%s", fsm.last_query[0] ? fsm.last_query : "-"); break;
        case FR_SAVE:  snprintf(v, n, "%s", !fsm.last_query[0] ? "-"
                                          : fs_query_is_saved(fsm.last_query) ? "saved" : "save >"); break;
        case FR_CLEAR: snprintf(v, n, "%d", fsm.n_recents); break;
        case FR_AUTO:  snprintf(v, n, "%s", fsm.autoplay ? "on" : "off"); break;
        // deliberately NOT on the results page: deleting a take should take a
        // deliberate trip to Setup, not a stray hold while browsing
        case FR_DROP:  snprintf(v, n, "%s", fs_audition_name()[0] ? fs_audition_name()
                                          : (fsm.cur_name[0] ? fsm.cur_name : "-")); break;
        default:       v[0] = 0; break;
    }
}

static void fs_setup_adj(int i, int dir)
{
    (void)dir;
    if (i == FR_AUTO) fsm.autoplay = !fsm.autoplay;
}

static int fs_setup_action(int i)
{
    switch (i) {
        case FR_NEW:
            return M_FS_ENTRY;
        case FR_SAVE:
            if (!fsm.last_query[0]) break;
            // press toggles: a saved query un-saves, so one row covers both
            if (fs_query_is_saved(fsm.last_query)) fs_query_unsave(fsm.last_query);
            else                                   fs_query_save(fsm.last_query);
            break;
        case FR_CLEAR:
            fsm.n_recents = 0;
            break;
        case FR_DROP:
            if (fs_audition_name()[0]) fs_audition_drop();
            else if (fsm.cur_name[0])  { fs_audition(fsm.cur_name); fs_audition_drop(); }
            break;
        default: break;
    }
    return 0;
}

static setup_menu_t fs_setup = {
    .items = fs_setup_items,
    .n = (int)(sizeof(fs_setup_items) / sizeof(fs_setup_items[0])),
    .title = "Freesound Setup",
    .aff_label = "Machine",
    .aff_target = M_MORE,
    .live_target = M_FS_LIVE,
    .render = fs_setup_val,
    .adjust = fs_setup_adj,
    .action = fs_setup_action,
};

static int fs_setup_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    return setup_menu_event(&fs_setup, event);
}

// ---- registration ---------------------------------------------------------
static void fs_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_FS_LIVE);    menusys_item_set_default_cb(_ms, M_FS_LIVE,    fs_live_handler);
    menusys_new_item(_ms, M_FS_ENTRY);   menusys_item_set_default_cb(_ms, M_FS_ENTRY,   fs_entry_handler);
    menusys_new_item(_ms, M_FS_RESULTS); menusys_item_set_default_cb(_ms, M_FS_RESULTS, fs_results_handler);
    menusys_new_item(_ms, M_FS_SETUP);   menusys_item_set_default_cb(_ms, M_FS_SETUP,   fs_setup_handler);
}

static int fs_main_event(int event, void *ev_data)
{
    (void)ev_data;
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW) {
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK;
        TFT_fillRect(0, fh + 12, _width, 24, _bg);
        _fg = ACCENT;
        char s[64];
        if (fsm.phase != FS_IDLE && fsm.phase != FS_DONE)
            snprintf(s, sizeof(s), "Freesound: %s %d%%", fs_phase_name(fsm.phase), fsm.progress);
        else if (fsm.last_query[0])
            snprintf(s, sizeof(s), "Freesound: \"%s\"", fsm.last_query);
        else
            snprintf(s, sizeof(s), "Freesound: search the library");
        while (s[0] && TFT_getStringWidth(s) > _width - 12) s[strlen(s) - 1] = 0;
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const fs_main_items[] = {"Live", "Setup"};
static const int fs_main_targets[] = {M_FS_LIVE, M_FS_SETUP};

const machine_ui_t fs_menu_ui = {
    .main_items = fs_main_items,
    .main_targets = fs_main_targets,
    .n_main = 2,
    .register_pages = fs_register_pages,
    .main_event = fs_main_event,
    .web_uris = fs_web_uris,
    .n_web_uris = FS_WEB_URIS_N,
    .boot_target = M_FS_LIVE,
    .setup = &fs_setup,
};
