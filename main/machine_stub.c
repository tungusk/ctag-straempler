#include <string.h>
#include <stdio.h>
#include "machine.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"

// Minimal machine: outputs silence. Exists so a build can ship without the
// sampler (the M0d proof) and as the SAFE FALLBACK when a machine fails to start.
//
// It has a UI, and that is not decoration (Arlo: "shouldn't the stub at least be
// able to open the menu"). The stub is what you land on when something has gone
// WRONG — a machine's start() refused, a registry is empty — and a fallback you
// cannot steer out of is not a fallback: with no pages registered the menu system
// had nothing to boot into, so the screen sat dead and System->Machine was
// unreachable. The safety net now says what happened and hands you the menu.

static esp_err_t stub_start(void) { return ESP_OK; }
static void stub_stop(void) {}

static void stub_process(int32_t out[MACHINE_BLOCK], const int32_t in[MACHINE_BLOCK], const machine_io_t *io)
{
    (void)in; (void)io;
    memset(out, 0, MACHINE_BLOCK * sizeof(int32_t));
}

static void stub_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK;
    _fg = TFT_WHITE;
    TFT_print("No machine", 8, 8);

    _fg = (color_t){150, 160, 190};
    int fh = TFT_getfontheight();
    TFT_print("This is the fallback machine: it plays", 8, 20 + fh);
    TFT_print("silence. A machine either failed to", 8, 20 + 2 * fh + 4);
    TFT_print("start, or none is loaded.", 8, 20 + 3 * fh + 8);

    _fg = (color_t){230, 170, 0};
    TFT_print("hold  ->  System > Machine", 8, 28 + 5 * fh);

    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("press: redraw   hold: menu", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int stub_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU:
        case EV_SHORT_PRESS:
            stub_redraw();
            break;
        case EV_LONG_PRESS:
            return M_MORE;          // the way OUT — the whole point of the page
        default:
            break;
    }
    return 0;
}

static void stub_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_STUB_HOME);
    menusys_item_set_default_cb(_ms, M_STUB_HOME, stub_handler);
}

static const machine_ui_t stub_ui = {
    .register_pages = stub_register_pages,
    .boot_target = M_STUB_HOME,
};

const machine_t machine_stub = {
    .name = "Stub",
    .start = stub_start,
    .stop = stub_stop,
    .process = stub_process,
    .ui = &stub_ui,
};
