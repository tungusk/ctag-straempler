// Radio UI — a Live page (state + station picker: turn to choose, press to play)
// and a tiny Setup (Stop + the Machine affordance to reach System).
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
#include "radio_priv.h"

static const color_t COL_STOP = {110, 110, 120};
static const color_t COL_BUF  = {230, 170, 0};
static const color_t COL_PLAY = {40, 200, 90};
static const color_t COL_ERR  = {220, 60, 40};

static const char *state_word(void)
{
    switch (rd.state) { case RADIO_PLAYING: return "PLAYING";
                        case RADIO_BUFFERING: return "BUFFERING";
                        case RADIO_ERROR: return "ERROR";
                        default: return "STOPPED"; }
}
static color_t state_col(void)
{
    switch (rd.state) { case RADIO_PLAYING: return COL_PLAY;
                        case RADIO_BUFFERING: return COL_BUF;
                        case RADIO_ERROR: return COL_ERR;
                        default: return COL_STOP; }
}

static int s_last_state = -1;

static void state_block(void)
{
    color_t c = state_col();
    int fh = TFT_getfontheight();
    int by = fh + 18, bh = 62;
    _bg = (color_t){ (uint8_t)(c.r / 6), (uint8_t)(c.g / 6), (uint8_t)(c.b / 6) };
    TFT_fillRect(10, by, _width - 20, bh, _bg);
    _fg = c; TFT_drawRect(10, by, _width - 20, bh, _fg);
    Font f = cfont; TFT_setFont(DEJAVU24_FONT, NULL);
    const char *w = state_word();
    TFT_print((char *)w, _width / 2 - TFT_getStringWidth((char *)w) / 2, by + bh / 2 - TFT_getfontheight() / 2);
    cfont = f;
    s_last_state = rd.state;
}

static void info_block(void)
{
    int fh = TFT_getfontheight();
    int y = fh + 90;
    _bg = TFT_BLACK; TFT_fillRect(0, y, _width, (fh + 6) * 2, _bg);

    // the station line: while stopped, show the SELECTED station (turn to pick);
    // while active, show what's playing
    bool browsing = (rd.state == RADIO_STOPPED || rd.state == RADIO_ERROR);
    const char *nm = browsing
                     ? rd_stations[rd.sel % rd_n_stations].name
                     : (rd.station[0] ? rd.station : "—");
    char s[48];
    if (browsing) snprintf(s, sizeof(s), "< %s >", nm);
    else          snprintf(s, sizeof(s), "%s", nm);
    _fg = TFT_WHITE;
    TFT_print(s, _width / 2 - TFT_getStringWidth(s) / 2, y);

    char t[84];
    if (rd.state == RADIO_ERROR)      snprintf(t, sizeof(t), "%s", rd.err);
    else if (rd.state == RADIO_PLAYING && rd.title[0]) snprintf(t, sizeof(t), "%.76s", rd.title);
    else if (rd.state == RADIO_PLAYING) snprintf(t, sizeof(t), "%d kbps  %d Hz  buf %d%%",
             rd.bitrate, rd.samprate, (int)((uint64_t)(rd.wpos - rd.rpos) * 100 / RADIO_RING_FRAMES));
    else if (rd.state == RADIO_BUFFERING) snprintf(t, sizeof(t), "buffering  %d%%",
             (int)((uint64_t)(rd.wpos - rd.rpos) * 100 / RADIO_LOW_WATER));
    else                              snprintf(t, sizeof(t), "press to play");
    _fg = (rd.state == RADIO_ERROR) ? COL_ERR : TFT_LIGHTGREY;
    TFT_print(t, _width / 2 - TFT_getStringWidth(t) / 2, y + fh + 4);
}

static void live_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Radio", 6, 4);
    state_block();
    info_block();
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn:station  press:play  hold:setup/stop", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int radio_live_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST:
            if (rd.state != s_last_state) state_block();
            info_block();
            break;
        // browse in STOPPED *and* ERROR — a failed connect must not freeze the
        // picker (press then retries the newly-selected station)
        case EV_FWD:
            if (rd.state == RADIO_STOPPED || rd.state == RADIO_ERROR) { rd.sel = (rd.sel + 1) % rd_n_stations; info_block(); }
            break;
        case EV_BWD:
            if (rd.state == RADIO_STOPPED || rd.state == RADIO_ERROR) { rd.sel = (rd.sel + rd_n_stations - 1) % rd_n_stations; info_block(); }
            break;
        case EV_SHORT_PRESS:
            radio_play_station(rd.sel);
            state_block(); info_block();
            break;
        case EV_LONG_PRESS: return M_RADIO_SETUP;
        default: break;
    }
    return 0;
}

// ---- Setup: Stop + Machine affordance --------------------------------------
static const char *setup_labels[] = { "Stop" };
#define RADIO_SETUP_N 1

static void setup_redraw(int pos)
{
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE; TFT_print("Radio Setup", 6, 4);
    menuTFTPrintAffordance("Machine", pos == -1);
    for (int i = 0; i < RADIO_SETUP_N; i++) {
        int y = fh + 20 + i * (fh + 8);
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
        TFT_print((char *)setup_labels[i], 8, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("press Stop to end the stream", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int radio_setup_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    static int pos = 0;
    switch (event) {
        case EV_ENTERED_MENU: pos = 0; setup_redraw(pos); break;
        case EV_FWD: pos++; if (pos >= RADIO_SETUP_N) pos = -1; setup_redraw(pos); break;
        case EV_BWD: pos--; if (pos < -1) pos = RADIO_SETUP_N - 1; setup_redraw(pos); break;
        case EV_SHORT_PRESS:
            if (pos == -1) return M_MORE;      // System affordance
            radio_stop_stream();               // pos 0 = Stop
            setup_redraw(pos);
            break;
        case EV_LONG_PRESS: return M_RADIO_LIVE;
        default: break;
    }
    return 0;
}

static void radio_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_RADIO_LIVE);  menusys_item_set_default_cb(_ms, M_RADIO_LIVE, radio_live_handler);
    menusys_new_item(_ms, M_RADIO_SETUP); menusys_item_set_default_cb(_ms, M_RADIO_SETUP, radio_setup_handler);
}

static int radio_main_event(int event, void *ev_data)
{
    (void)ev_data;
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW) {
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        _fg = state_col();
        char s[40];
        snprintf(s, sizeof(s), "Radio: %s", state_word());
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

extern const httpd_uri_t radio_web_uris[];

static const char *const radio_main_items[] = { "Live", "Setup" };
static const int radio_main_targets[] = { M_RADIO_LIVE, M_RADIO_SETUP };

const machine_ui_t radio_menu_ui = {
    .main_items = radio_main_items,
    .main_targets = radio_main_targets,
    .n_main = 2,
    .register_pages = radio_register_pages,
    .main_event = radio_main_event,
    .boot_target = M_RADIO_LIVE,
    .web_uris = radio_web_uris,
    .n_web_uris = 5,
};
