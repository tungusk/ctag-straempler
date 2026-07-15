// Editor UI — minimal on-device status (the work is web-driven, like Freesound):
// Live shows the current/last job; Setup reaches System.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include <esp_http_server.h>
#include "machine.h"
#include "editor_priv.h"

static const char *state_word(void)
{
    switch (ed.state) { case ED_RUNNING: return "WORKING"; case ED_DONE: return "DONE";
                        case ED_ERR: return "ERROR"; default: return "IDLE"; }
}
static color_t state_col(void)
{
    switch (ed.state) { case ED_RUNNING: return (color_t){230,170,0}; case ED_DONE: return (color_t){40,200,90};
                        case ED_ERR: return (color_t){220,60,40}; default: return (color_t){110,110,120}; }
}

static void live_body(void)
{
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; TFT_fillRect(0, fh + 16, _width, _height - fh - 40, _bg);
    color_t c = state_col();
    Font f = cfont; TFT_setFont(DEJAVU24_FONT, NULL);
    _fg = c;
    const char *w = state_word();
    TFT_print((char*)w, _width/2 - TFT_getStringWidth((char*)w)/2, fh + 26);
    cfont = f;
    int y = fh + 62;
    char s[56];
    if (ed.state == ED_RUNNING) snprintf(s, sizeof(s), "%s  %s  %d%%", ed.op >= 0 && ed.op < OP_N ? ed_op_names[ed.op] : "", ed.src, ed.progress);
    else if (ed.state == ED_DONE) snprintf(s, sizeof(s), "%s -> %s", ed.src, ed.out);
    else if (ed.state == ED_ERR)  snprintf(s, sizeof(s), "%s", ed.err);
    else                          snprintf(s, sizeof(s), "pick a sample + op in the web Editor tab");
    _fg = (ed.state == ED_ERR) ? (color_t){220,60,40} : TFT_LIGHTGREY;
    TFT_print(s, _width/2 - TFT_getStringWidth(s)/2, y);
}

static void live_full_redraw(void)
{
    TFT_resetclipwin(); TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE; TFT_print("Editor", 6, 4);
    live_body();
    _fg = (color_t){90,90,90}; TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("non-destructive: normalize / reverse / fade / trim -> new take", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int editor_live_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST: live_body(); break;
        case EV_LONG_PRESS: return M_EDITOR_SETUP;
        default: break;
    }
    return 0;
}

static int editor_setup_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU: {
            TFT_resetclipwin(); TFT_fillScreen(TFT_BLACK);
            _bg = TFT_BLACK; _fg = TFT_WHITE; TFT_print("Editor Setup", 6, 4);
            menuTFTPrintAffordance("Machine", true);
            _fg = (color_t){90,90,90}; TFT_setFont(DEF_SMALL_FONT, NULL);
            TFT_print("press to switch machine; edits are driven from the web", 8, _height - TFT_getfontheight() - 1);
            TFT_setFont(DEFAULT_FONT, NULL);
            break;
        }
        case EV_SHORT_PRESS: return M_MORE;
        case EV_LONG_PRESS:  return M_EDITOR_LIVE;
        default: break;
    }
    return 0;
}

static void editor_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_EDITOR_LIVE);  menusys_item_set_default_cb(_ms, M_EDITOR_LIVE, editor_live_handler);
    menusys_new_item(_ms, M_EDITOR_SETUP); menusys_item_set_default_cb(_ms, M_EDITOR_SETUP, editor_setup_handler);
}

static int editor_main_event(int event, void *ev_data)
{
    (void)ev_data;
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW) {
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        _fg = state_col();
        char s[40]; snprintf(s, sizeof(s), "Editor: %s", state_word());
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

extern const httpd_uri_t editor_web_uris[];

static const char *const editor_main_items[] = { "Live", "Setup" };
static const int editor_main_targets[] = { M_EDITOR_LIVE, M_EDITOR_SETUP };

const machine_ui_t editor_menu_ui = {
    .main_items = editor_main_items,
    .main_targets = editor_main_targets,
    .n_main = 2,
    .register_pages = editor_register_pages,
    .main_event = editor_main_event,
    .boot_target = M_EDITOR_LIVE,
    .web_uris = editor_web_uris,
    .n_web_uris = 2,
};
