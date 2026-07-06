// M4 granular UI — Live view (waveform + moving cloud position + grain
// activity) and a Setup page (Grain / Density / Spray / Spread / Sample).
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
        case EV_LONG_PRESS: return M_MAIN;
        default: break;
    }
    return 0;
}

// ---- Setup ----------------------------------------------------------------
static const char *setup_labels[] = {"Grain ms", "Density", "Spray", "Spread", "Sample"};
#define GR_SETUP_N 5

static char s_samples[32][24];
static int  s_n_samples = 0, s_sample_idx = 0;

static void refresh_samples(void){
    s_n_samples = granular_list_samples(s_samples, 32);
    s_sample_idx = 0;
    for (int i = 0; i < s_n_samples; i++) if (strcmp(s_samples[i], gr.sample) == 0) { s_sample_idx = i; break; }
}

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE;
    TFT_print("Granular Setup", 6, 4);
    for (int i = 0; i < GR_SETUP_N; i++){
        int y = fh + 14 + i * (fh + 8);
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
        TFT_print((char*)setup_labels[i], 8, y);
        char v[24];
        switch(i){
            case 0: snprintf(v, sizeof(v), "%d", gr.grain_ms); break;
            case 1: snprintf(v, sizeof(v), "%d", gr.density); break;
            case 2: snprintf(v, sizeof(v), "%d", gr.spray); break;
            case 3: snprintf(v, sizeof(v), "%d", gr.spread); break;
            case 4: snprintf(v, sizeof(v), "%s", gr.sample[0] ? gr.sample : "(none)"); break;
        }
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
}

static void adj(int i, int dir){
    switch(i){
        case 0: gr.grain_ms += dir * 10; if(gr.grain_ms < 10) gr.grain_ms = 10; if(gr.grain_ms > 500) gr.grain_ms = 500; break;
        case 1: gr.density  += dir * 2;  if(gr.density < 1) gr.density = 1;      if(gr.density > 120) gr.density = 120; break;
        case 2: gr.spray    += dir * 5;  if(gr.spray < 0) gr.spray = 0;          if(gr.spray > 100) gr.spray = 100; break;
        case 3: gr.spread   += dir * 5;  if(gr.spread < 0) gr.spread = 0;        if(gr.spread > 100) gr.spread = 100; break;
    }
}

static int gran_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; refresh_samples(); setup_redraw(pos, sel); break;
        case EV_FWD:
            if(sel){ if(pos < 4) adj(pos, +1); }
            else pos = (pos + 1) % GR_SETUP_N;
            setup_redraw(pos, sel);
            break;
        case EV_BWD:
            if(sel){ if(pos < 4) adj(pos, -1); }
            else pos = (pos + GR_SETUP_N - 1) % GR_SETUP_N;
            setup_redraw(pos, sel);
            break;
        case EV_SHORT_PRESS:
            if(pos == 4){ refresh_samples(); return M_GRAN_LOAD; }
            sel = !sel; setup_redraw(pos, sel);
            break;
        case EV_LONG_PRESS: return M_MAIN;
        default: break;
    }
    return 0;
}

// ---- Load browser (centered, big selected name) ---------------------------
static void load_redraw(void){
    TFT_resetclipwin(); TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; _fg = TFT_LIGHTGREY;
    char h[32]; snprintf(h, sizeof(h), "Load Sample  (%d/%d)", s_n_samples ? s_sample_idx + 1 : 0, s_n_samples);
    TFT_print(h, _width / 2 - TFT_getStringWidth(h) / 2, 4);
    if (!s_n_samples){ char *m = "no samples in usr/"; TFT_print(m, _width / 2 - TFT_getStringWidth(m) / 2, _height / 2); return; }
    int cy = _height / 2;
    Font f = cfont; TFT_setFont(DEJAVU24_FONT, NULL);
    int bigfh = TFT_getfontheight(); _fg = TFT_WHITE;
    char *selnm = s_samples[s_sample_idx];
    TFT_print(selnm, _width / 2 - TFT_getStringWidth(selnm) / 2, cy - bigfh / 2);
    cfont = f;
    _fg = (color_t){110, 110, 110};
    for (int k = 1; k <= 4; k++){
        int up = s_sample_idx - k, dn = s_sample_idx + k;
        int yup = cy - bigfh / 2 - k * (fh + 4) - 4;
        int ydn = cy + bigfh / 2 + (k - 1) * (fh + 4) + 6;
        if (up >= 0){ char *n = s_samples[up]; TFT_print(n, _width / 2 - TFT_getStringWidth(n) / 2, yup); }
        if (dn < s_n_samples){ char *n = s_samples[dn]; TFT_print(n, _width / 2 - TFT_getStringWidth(n) / 2, ydn); }
    }
    _fg = (color_t){90, 90, 90}; TFT_setFont(DEF_SMALL_FONT, NULL);
    char *hint = "turn:browse  press:load  hold:cancel";
    TFT_print(hint, _width / 2 - TFT_getStringWidth(hint) / 2, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int gran_load_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: refresh_samples(); load_redraw(); break;
        case EV_FWD: if(s_n_samples){ s_sample_idx = (s_sample_idx + 1) % s_n_samples; load_redraw(); } break;
        case EV_BWD: if(s_n_samples){ s_sample_idx = (s_sample_idx + s_n_samples - 1) % s_n_samples; load_redraw(); } break;
        case EV_SHORT_PRESS: if(s_n_samples) granular_load(s_samples[s_sample_idx]); return M_GRAN_SETUP;
        case EV_LONG_PRESS: return M_GRAN_SETUP;
        default: break;
    }
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
};
