// M3 slicer UI — a Live view (waveform + slice grid, current/selected slice
// highlighted) and a Setup page (Slices / Sample / Auto / Reverse). Redraws
// only when the playing or selected slice changes, so it stays flicker-free.
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
#include "slicer_priv.h"

static const color_t BG      = {5, 9, 28};
static const color_t WAVE    = {70, 110, 180};
static const color_t GRID    = {40, 60, 110};
static const color_t SEL_COL = {40, 200, 230};   // selected slice
static const color_t CUR_COL = {40, 200, 90};    // playing slice

// waveform box
#define WX 8
#define WY (TFT_getfontheight() + 16)
#define WW (_width - 16)
#define WH 120

static int s_last_cur = -1, s_last_sel = -1, s_last_slices = -1;
static char s_msg[24];

// repaint a single slice column (waveform + grid + its highlight) without
// clearing the whole screen — so moving the highlight doesn't black out
static void repaint_slice(int s){
    if (s < 0 || s >= sl.n_slices || sl.len == 0) return;
    int x0 = WX + s * WW / sl.n_slices;
    int x1 = WX + (s + 1) * WW / sl.n_slices;
    int cy = WY + WH / 2;
    bool is_cur = sl.playing && s == sl.cur;
    _bg = is_cur ? (color_t){20, 40, 50} : BG;
    TFT_fillRect(x0, WY, x1 - x0, WH, _bg);
    for (int c = 0; c < sl.peak_n; c++){
        int x = WX + c * WW / sl.peak_n;
        if (x < x0 || x >= x1) continue;
        int h = sl.peaks[c] * (WH / 2) / 31;
        if (h < 1 && sl.peaks[c] > 0) h = 1;
        TFT_drawLine(x, cy - h, x, cy + h, WAVE);
    }
    TFT_drawLine(x0, WY, x0, WY + WH, GRID);
    TFT_drawLine(x1, WY, x1, WY + WH, GRID);
    if (s == sl.sel) TFT_drawRect(x0, WY, x1 - x0, WH, SEL_COL);
    if (is_cur)      TFT_drawRect(x0, WY, x1 - x0, WH, CUR_COL);
}

// move highlights by repainting only the vacated + newly-marked slices
static void live_update_highlights(void){
    repaint_slice(s_last_cur);
    repaint_slice(s_last_sel);
    repaint_slice(sl.cur);
    repaint_slice(sl.sel);
    s_last_cur = sl.cur;
    s_last_sel = sl.sel;
}

static void draw_slice_region(int s, color_t c, bool fill){
    if (sl.n_slices < 1) return;
    int x0 = WX + s * WW / sl.n_slices;
    int x1 = WX + (s + 1) * WW / sl.n_slices;
    _fg = c;
    if (fill){
        _bg = (color_t){20, 40, 50};
        TFT_fillRect(x0 + 1, WY, x1 - x0 - 2, WH, _bg);
    }
    TFT_drawRect(x0, WY, x1 - x0, WH, c);
}

static void draw_waveform(void){
    int cy = WY + WH / 2;
    // current slice block gets a tinted fill first (background layer)
    if (sl.playing) draw_slice_region(sl.cur, CUR_COL, true);
    // peaks
    _fg = WAVE;
    for (int c = 0; c < sl.peak_n; c++){
        int x = WX + c * WW / (sl.peak_n ? sl.peak_n : 1);
        int h = sl.peaks[c] * (WH / 2) / 31;
        if (h < 1 && sl.peaks[c] > 0) h = 1;
        TFT_drawLine(x, cy - h, x, cy + h, WAVE);
    }
    // slice division grid
    for (int s = 0; s <= sl.n_slices; s++){
        int x = WX + s * WW / sl.n_slices;
        TFT_drawLine(x, WY, x, WY + WH, GRID);
    }
    // selected slice outline (bright) + current playing outline
    draw_slice_region(sl.sel, SEL_COL, false);
    if (sl.playing) draw_slice_region(sl.cur, CUR_COL, false);
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    char hdr[40];
    snprintf(hdr, sizeof(hdr), "Slicer  %s  x%d", sl.sample[0] ? sl.sample : "(none)", sl.n_slices);
    TFT_print(hdr, 6, 4);
    _bg = BG;
    TFT_fillRect(WX - 2, WY - 2, WW + 4, WH + 4, _bg);
    if (sl.len == 0){
        _fg = TFT_LIGHTGREY;
        TFT_print("no sample - load one in Setup", WX + 10, WY + WH / 2);
    } else {
        draw_waveform();
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    char foot[80];
    snprintf(foot, sizeof(foot), "slice %d/%d  %s%s  turn:sel press:fire hold:exit",
             sl.sel + 1, sl.n_slices, sl.auto_on ? "AUTO " : "", sl.reverse ? "REV" : "");
    TFT_print(foot, 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
    s_last_cur = sl.cur; s_last_sel = sl.sel; s_last_slices = sl.n_slices;
}

static int slicer_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU:
            live_full_redraw();
            break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST:
            if (sl.n_slices != s_last_slices) live_full_redraw();   // grid changed
            else if (sl.cur != s_last_cur || sl.sel != s_last_sel)
                live_update_highlights();                           // just move highlights
            break;
        case EV_FWD:
            sl.sel = (sl.sel + 1) % sl.n_slices;
            live_update_highlights();
            break;
        case EV_BWD:
            sl.sel = (sl.sel + sl.n_slices - 1) % sl.n_slices;
            live_update_highlights();
            break;
        case EV_SHORT_PRESS:
            sl.cmd_fire = 1;            // audition the selected slice
            break;
        case EV_LONG_PRESS:
            return M_MAIN;
        default: break;
    }
    return 0;
}

// ---- Setup page -----------------------------------------------------------
static const char *setup_labels[] = {"Slices", "Sample", "Auto", "Reverse"};
#define SL_SETUP_N 4

// cached sample list for the Sample row cycler
static char s_samples[32][24];
static int  s_n_samples = 0, s_sample_idx = 0;

static void setup_refresh_samples(void){
    s_n_samples = slicer_list_samples(s_samples, 32);
    s_sample_idx = 0;
    for (int i = 0; i < s_n_samples; i++)
        if (strcmp(s_samples[i], sl.sample) == 0) { s_sample_idx = i; break; }
}

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE;
    TFT_print("Slicer Setup", 6, 4);
    for (int i = 0; i < SL_SETUP_N; i++){
        int y = fh + 14 + i * (fh + 8);
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
        TFT_print((char*)setup_labels[i], 8, y);
        char v[24];
        switch(i){
            case 0: snprintf(v, sizeof(v), "%d", sl.n_slices); break;
            case 1: snprintf(v, sizeof(v), "%s", sl.sample[0] ? sl.sample : "(none)"); break;
            case 2: snprintf(v, sizeof(v), "%s", sl.auto_on ? "ON" : "OFF"); break;
            case 3: snprintf(v, sizeof(v), "%s", sl.reverse ? "ON" : "OFF"); break;
        }
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
    if (s_msg[0]){ _fg = TFT_LIGHTGREY; TFT_print(s_msg, 8, _height - fh - 2); }
}

static void cycle_slices(int dir){
    int n = sl.n_slices;
    if (dir > 0) n = (n >= 32) ? 8 : n * 2;
    else         n = (n <= 8) ? 32 : n / 2;
    sl.n_slices = n;
    if (sl.sel >= n) sl.sel = n - 1;
}

static int slicer_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU:
            pos = 0; sel = 0; s_msg[0] = 0; setup_refresh_samples();
            setup_redraw(pos, sel);
            break;
        case EV_FWD:
            if(sel){
                if(pos==0) cycle_slices(+1);
                else if(pos==1){ if(s_n_samples){ s_sample_idx = (s_sample_idx+1)%s_n_samples; snprintf(s_msg,sizeof(s_msg),"press to load"); } }
                else if(pos==2) sl.auto_on = !sl.auto_on;
                else if(pos==3) sl.reverse = !sl.reverse;
            } else pos = (pos + 1) % SL_SETUP_N;
            setup_redraw(pos, sel);
            break;
        case EV_BWD:
            if(sel){
                if(pos==0) cycle_slices(-1);
                else if(pos==1){ if(s_n_samples){ s_sample_idx = (s_sample_idx+s_n_samples-1)%s_n_samples; snprintf(s_msg,sizeof(s_msg),"press to load"); } }
                else if(pos==2) sl.auto_on = !sl.auto_on;
                else if(pos==3) sl.reverse = !sl.reverse;
            } else pos = (pos + SL_SETUP_N - 1) % SL_SETUP_N;
            setup_redraw(pos, sel);
            break;
        case EV_SHORT_PRESS:
            if(pos == 1 && sel && s_n_samples){
                // load the highlighted sample
                int r = slicer_load(s_samples[s_sample_idx]);
                snprintf(s_msg, sizeof(s_msg), "%s", r == 0 ? "loaded" : "load failed");
                setup_redraw(pos, sel);
            } else {
                sel = !sel; s_msg[0] = 0; setup_redraw(pos, sel);
            }
            break;
        case EV_LONG_PRESS: return M_MAIN;
        default: break;
    }
    return 0;
}

// ---- registration ---------------------------------------------------------
static void slicer_register_pages(void *menusys){
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_SLICER_LIVE);
    menusys_item_set_default_cb(_ms, M_SLICER_LIVE, slicer_live_handler);
    menusys_new_item(_ms, M_SLICER_SETUP);
    menusys_item_set_default_cb(_ms, M_SLICER_SETUP, slicer_setup_handler);
}

static int slicer_main_event(int event, void *ev_data){
    // compact status on the main screen (below the core menu bar)
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW){
        int fh = TFT_getfontheight();
        _bg = BG; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        _fg = TFT_WHITE;
        char s[64];
        snprintf(s, sizeof(s), "Slicer: %s  x%d  slice %d",
                 sl.sample[0] ? sl.sample : "(none)", sl.n_slices, sl.cur + 1);
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const slicer_main_items[] = {"Live", "Setup"};
static const int slicer_main_targets[] = {M_SLICER_LIVE, M_SLICER_SETUP};

const machine_ui_t slicer_menu_ui = {
    .main_items = slicer_main_items,
    .main_targets = slicer_main_targets,
    .n_main = 2,
    .register_pages = slicer_register_pages,
    .main_event = slicer_main_event,
};
