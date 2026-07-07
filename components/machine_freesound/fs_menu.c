// Freesound machine UI — a single status page: where to point the browser,
// whether the API key is set, and live pipeline phase/progress.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <esp_http_server.h>
#include "menusys.h"
#include "menu_types.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "wifi.h"
#include "fs_auth.h"
#include "fs_priv.h"

extern const httpd_uri_t fs_web_uris[];
extern const int fs_web_n_uris;

static const color_t ACCENT = {230, 90, 40};

static int s_last_phase = -1, s_last_prog = -1;

static void status_block(void)
{
    int fh = TFT_getfontheight();
    int y = fh * 4 + 40;
    _bg = TFT_BLACK;
    TFT_fillRect(0, y, _width, (fh + 6) * 2, _bg);
    char s[128];
    if (fsm.phase == FS_ERROR) {
        _fg = (color_t){220, 60, 60};
        snprintf(s, sizeof(s), "error: %s", fsm.err);
    } else if (fsm.phase == FS_IDLE) {
        _fg = TFT_LIGHTGREY;
        snprintf(s, sizeof(s), "idle");
    } else {
        _fg = ACCENT;
        snprintf(s, sizeof(s), "%s %s %d%%", fsm.cur_name[0] ? fsm.cur_name : "-",
                 fs_phase_name(fsm.phase), fsm.progress);
    }
    TFT_print(s, 8, y);
    s_last_phase = fsm.phase;
    s_last_prog = fsm.progress;
}

static void live_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK;
    _fg = TFT_WHITE;
    TFT_print("Freesound", 6, 4);

    int fh = TFT_getfontheight();
    char ip[20] = {0};
    wifiGetIPString(ip, sizeof(ip));
    char s[128];
    _fg = TFT_LIGHTGREY;
    snprintf(s, sizeof(s), "browser: http://%s", ip[0] ? ip : "(no wifi)");
    TFT_print(s, 8, fh + 16);
    TFT_print("open the Freesound tab", 8, fh * 2 + 22);

    _fg = fs_auth_ok() ? (color_t){40, 160, 90} : (color_t){220, 60, 60};
    TFT_print(fs_auth_ok() ? "API key: set" : "API key: MISSING (web Settings)", 8, fh * 3 + 32);

    status_block();

    if (fsm.last_query[0]) {
        _fg = (color_t){110, 110, 110};
        snprintf(s, sizeof(s), "last search: %s", fsm.last_query);
        TFT_print(s, 8, fh * 6 + 52);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("search + download land in the usr/ library", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int fs_live_handler(int it_id, int event, void *ev_data)
{
    switch (event) {
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST:
            if (fsm.phase != s_last_phase || fsm.progress != s_last_prog) status_block();
            break;
        case EV_LONG_PRESS: return M_MAIN;
        default: break;
    }
    return 0;
}

// ---- registration ---------------------------------------------------------
static void fs_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_FS_LIVE);
    menusys_item_set_default_cb(_ms, M_FS_LIVE, fs_live_handler);
}

static int fs_main_event(int event, void *ev_data)
{
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW) {
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK;
        TFT_fillRect(0, fh + 12, _width, 24, _bg);
        _fg = ACCENT;
        char s[64];
        if (fsm.phase == FS_IDLE || fsm.phase == FS_DONE)
            snprintf(s, sizeof(s), "Freesound: web-driven (see Status)");
        else
            snprintf(s, sizeof(s), "Freesound: %s %d%%", fs_phase_name(fsm.phase), fsm.progress);
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const fs_main_items[] = {"Status"};
static const int fs_main_targets[] = {M_FS_LIVE};

const machine_ui_t fs_menu_ui = {
    .main_items = fs_main_items,
    .main_targets = fs_main_targets,
    .n_main = 1,
    .register_pages = fs_register_pages,
    .main_event = fs_main_event,
    .web_uris = fs_web_uris,
    .n_web_uris = 4,
};
