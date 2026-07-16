// M4 granular UI — Live view (waveform + moving cloud position + grain
// activity) and a Setup page (Grain / Density / Spray / Spread / Sample).
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "sample_ram.h"
#include "sample_browser.h"
#include "setup_menu.h"
#include "granular_priv.h"

static const color_t BG   = {5, 9, 28};
static const color_t WAVE = {70, 110, 180};
static const color_t POSC = {40, 200, 230};   // cloud position marker

#define WX 8
#define WY 8
#define WW (_width - 16)
#define WH 150
#define INFO_Y (WY + WH + 6)

static int s_last_posx = -1;

static void gran_draw_wave(void){
    _bg = BG;
    TFT_fillRect(WX - 2, WY - 2, WW + 4, WH + 4, _bg);
    if (gr.len == 0){ _fg = TFT_LIGHTGREY; TFT_print("no sample - load one in Setup", WX + 10, WY + WH / 2); return; }
    int cy = WY + WH / 2;
    for (int c = 0; c < gr.peak_n; c++){
        int x = WX + c * WW / (gr.peak_n ? gr.peak_n : 1);
        int h = gr.peaks[c] * (WH / 2) / 31;
        if (h < 1 && gr.peaks[c] > 0) h = 1;
        TFT_drawLine(x, cy - h, x, cy + h, WAVE);
    }
    s_last_posx = -1;
}

static int pos_x(void){
    if (gr.len == 0) return WX;
    return WX + (int)((uint64_t)gr.base_pos * WW / gr.len);
}

// redraw just the moving cloud-position marker (erase old column, draw new)
static void gran_update_marker(void){
    if (gr.len == 0) return;
    int cy = WY + WH / 2;
    int px = pos_x();
    if (px == s_last_posx) return;
    // erase old marker column by repainting its waveform slice
    if (s_last_posx >= WX){
        _bg = BG;
        TFT_fillRect(s_last_posx, WY, 3, WH, _bg);
        for (int c = 0; c < gr.peak_n; c++){
            int x = WX + c * WW / gr.peak_n;
            if (x < s_last_posx || x >= s_last_posx + 3) continue;
            int h = gr.peaks[c] * (WH / 2) / 31; if (h < 1 && gr.peaks[c] > 0) h = 1;
            TFT_drawLine(x, cy - h, x, cy + h, WAVE);
        }
    }
    TFT_fillRect(px, WY, 2, WH, POSC);
    s_last_posx = px;
}

static void gran_draw_info(void){
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; TFT_fillRect(0, INFO_Y, _width, fh + 4, _bg);
    _fg = TFT_WHITE;
    char s[56];
    snprintf(s, sizeof(s), "%s  gr %dms  dns %d  grains %d",
             gr.sample[0] ? gr.sample : "(none)", gr.grain_ms, gr.density, gr.active_count);
    TFT_print(s, 6, INFO_Y + 2);
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    gran_draw_wave();
    gran_draw_info();
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("knob6:pos  knob7:pitch  TR1:freeze  hold:exit", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int gran_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST:
            gran_update_marker();
            gran_draw_info();
            break;
        case EV_LONG_PRESS: return M_GRAN_SETUP;   // toggle Live -> Setup
        default: break;
    }
    return 0;
}

// ---- Setup ----------------------------------------------------------------
static const setup_item_t gr_setup_items[] = {
    {"Grain ms", ST_RANGE},
    {"Density",  ST_RANGE},
    {"Spray",    ST_RANGE},
    {"Spread",   ST_RANGE},
    {"Sample",   ST_ACTION},
};

static void gr_render(int i, char *v, size_t n){
    switch(i){
        case 0: snprintf(v, n, "%d", gr.grain_ms); break;
        case 1: snprintf(v, n, "%d", gr.density); break;
        case 2: snprintf(v, n, "%d", gr.spray); break;
        case 3: snprintf(v, n, "%d", gr.spread); break;
        case 4: snprintf(v, n, "%s", gr.sample[0] ? gr.sample : "(none)"); break;
    }
}

static void gr_adj(int i, int dir){
    switch(i){
        case 0: gr.grain_ms += dir * 10; if(gr.grain_ms < 10) gr.grain_ms = 10; if(gr.grain_ms > 500) gr.grain_ms = 500; break;
        case 1: gr.density  += dir * 2;  if(gr.density < 1) gr.density = 1;      if(gr.density > 120) gr.density = 120; break;
        case 2: gr.spray    += dir * 5;  if(gr.spray < 0) gr.spray = 0;          if(gr.spray > 100) gr.spray = 100; break;
        case 3: gr.spread   += dir * 5;  if(gr.spread < 0) gr.spread = 0;        if(gr.spread > 100) gr.spread = 100; break;
    }
}

static int gr_setup_action(int i){ if(i == 4) return M_GRAN_LOAD; return 0; }

static setup_menu_t gr_setup = {
    .items = gr_setup_items, .n = 5,
    .title = "Granular Setup",
    .aff_label = "Machine", .aff_target = M_MORE,
    .live_target = M_GRAN_LIVE,
    .render = gr_render, .adjust = gr_adj, .action = gr_setup_action,
};

static int gran_setup_handler(int it_id, int event, void *ev_data){
    (void)it_id; (void)ev_data;
    return setup_menu_event(&gr_setup, event);
}

// ---- Load browser: the shared two-level widget (folders -> big-name list) --
static int gran_load_handler(int it_id, int event, void *ev_data){
    if (event == EV_ENTERED_MENU){ sample_browser_enter(false, "Load Sample", gr.sample); return 0; }
    int r = sample_browser_event(event);
    if (r == 1){ granular_load((char*)sample_browser_selected()); return M_GRAN_SETUP; }
    if (r == 2) return M_GRAN_SETUP;
    return 0;
}

// ---- registration ---------------------------------------------------------
static void gran_register_pages(void *menusys){
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_GRAN_LIVE);  menusys_item_set_default_cb(_ms, M_GRAN_LIVE, gran_live_handler);
    menusys_new_item(_ms, M_GRAN_SETUP); menusys_item_set_default_cb(_ms, M_GRAN_SETUP, gran_setup_handler);
    menusys_new_item(_ms, M_GRAN_LOAD);  menusys_item_set_default_cb(_ms, M_GRAN_LOAD, gran_load_handler);
}

static int gran_main_event(int event, void *ev_data){
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW){
        int fh = TFT_getfontheight();
        _bg = BG; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        _fg = TFT_WHITE;
        char s[48];
        snprintf(s, sizeof(s), "Granular: %s  grains %d", gr.sample[0] ? gr.sample : "(none)", gr.active_count);
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const gran_main_items[] = {"Live", "Setup"};
static const int gran_main_targets[] = {M_GRAN_LIVE, M_GRAN_SETUP};

const machine_ui_t granular_menu_ui = {
    .main_items = gran_main_items,
    .main_targets = gran_main_targets,
    .n_main = 2,
    .register_pages = gran_register_pages,
    .main_event = gran_main_event,
    .boot_target = M_GRAN_LIVE,
};
