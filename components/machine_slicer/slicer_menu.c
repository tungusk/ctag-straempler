// M3 slicer UI — a Live view (waveform + slice grid, current/selected slice
// highlighted) and a Setup page (Slices / Sample / Auto / Reverse). Redraws
// only when the playing or selected slice changes, so it stays flicker-free.
#include <stdio.h>
#include <string.h>
#include <esp_http_server.h>
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
#include "slicer_priv.h"

static const color_t BG      = {5, 9, 28};
static const color_t WAVE    = {70, 110, 180};
static const color_t GRID    = {40, 60, 110};
static const color_t SEL_COL = {40, 200, 230};   // selected slice
static const color_t CUR_COL = {40, 200, 90};    // playing slice

// waveform box — near the top now; sample/grid info sits below it
#define WX 8
#define WY 8
#define WW (_width - 16)
#define WH 150
#define INFO_Y (WY + WH + 6)

static int s_last_cur = -1, s_last_sel = -1, s_last_slices = -1;
static char s_msg[24];

// x pixel of slice boundary s (slices are non-uniform in transient mode)
static int slice_x(int s){
    if (sl.len == 0) return WX;
    if (s < 0) s = 0;
    if (s > sl.n_slices) s = sl.n_slices;
    return WX + (int)((uint64_t)sl.slice_pt[s] * WW / sl.len);
}

// repaint a single slice column (waveform + grid + its highlight) without
// clearing the whole screen. cur/sel are passed in (snapshotted by the caller)
// so the drawn state matches the cache even though the audio thread writes
// sl.cur/sl.sel asynchronously — otherwise a stale outline can linger.
static void repaint_slice(int s, int cur, int sel){
    if (s < 0 || s >= sl.n_slices || sl.len == 0) return;
    int x0 = slice_x(s);
    int x1 = slice_x(s + 1);
    int cy = WY + WH / 2;
    bool is_cur = sl.playing && s == cur;
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
    if (s == sel) TFT_drawRect(x0, WY, x1 - x0, WH, SEL_COL);
    if (is_cur)   TFT_drawRect(x0, WY, x1 - x0, WH, CUR_COL);
}

// sample name + grid size + live slice number, below the waveform
static void draw_info_line(void){
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; TFT_fillRect(0, INFO_Y, _width, fh + 4, _bg);
    _fg = TFT_WHITE;
    char s[64];
    snprintf(s, sizeof(s), "%s   x%d   slice %d/%d   %s%s",
             sl.sample[0] ? sl.sample : "(none)", sl.n_slices, sl.sel + 1, sl.n_slices,
             sl.auto_on ? "AUTO " : "", sl.reverse ? "REV" : "");
    TFT_print(s, 6, INFO_Y + 2);
}

// move highlights by repainting only the vacated + newly-marked slices
static void live_update_highlights(void){
    int cur = sl.cur, sel = sl.sel;     // one snapshot for draw AND cache
    repaint_slice(s_last_cur, cur, sel);
    repaint_slice(s_last_sel, cur, sel);
    repaint_slice(cur, cur, sel);
    repaint_slice(sel, cur, sel);
    s_last_cur = cur;
    s_last_sel = sel;
    draw_info_line();                   // keep the slice number current
}

static void draw_slice_region(int s, color_t c, bool fill){
    if (sl.n_slices < 1) return;
    int x0 = slice_x(s);
    int x1 = slice_x(s + 1);
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
    // slice division grid (boundaries may be non-uniform in transient mode)
    for (int s = 0; s <= sl.n_slices; s++){
        int x = slice_x(s);
        TFT_drawLine(x, WY, x, WY + WH, GRID);
    }
    // selected slice outline (bright) + current playing outline
    draw_slice_region(sl.sel, SEL_COL, false);
    if (sl.playing) draw_slice_region(sl.cur, CUR_COL, false);
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = BG;
    TFT_fillRect(WX - 2, WY - 2, WW + 4, WH + 4, _bg);
    if (sl.len == 0){
        _fg = TFT_LIGHTGREY;
        TFT_print("no sample - load one in Setup", WX + 10, WY + WH / 2);
    } else {
        draw_waveform();
    }
    draw_info_line();
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn:select  press:fire  hold:exit", 6, _height - TFT_getfontheight() - 1);
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
            return M_SLICER_SETUP;   // toggle Live -> Setup (no hub)
        default: break;
    }
    return 0;
}

// ---- Setup page -----------------------------------------------------------
static const char *setup_labels[] = {"Mode", "Slices", "Sensitivity", "Sample", "Auto", "Reverse", "Load Mono"};
#define SL_SETUP_N 7

// shared sorted library list (sample_ram) — big cards hold >200 samples
static char (*s_samples)[24] = NULL;
static int  s_n_samples = 0, s_sample_idx = 0;

static void setup_refresh_samples(void){
    s_n_samples = sample_list_shared(&s_samples);
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
    menuTFTPrintAffordance("Machine", pos == -1);
    for (int i = 0; i < SL_SETUP_N; i++){
        int y = fh + 14 + i * (fh + 8);
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
        TFT_print((char*)setup_labels[i], 8, y);
        char v[24];
        switch(i){
            case 0: snprintf(v, sizeof(v), "%s", sl.ot_active ? "OT"
                                : (sl.transient_mode ? "Transient" : "Grid")); break;
            case 1:
                if(sl.slice_target == 0) snprintf(v, sizeof(v), "Auto");
                else { snprintf(v, sizeof(v), "%d", sl.slice_target); }
                break;
            case 2: snprintf(v, sizeof(v), "%d", sl.sensitivity); break;
            case 3: snprintf(v, sizeof(v), "%s", sl.sample[0] ? sl.sample : "(none)"); break;
            case 4: snprintf(v, sizeof(v), "%s", sl.auto_on ? "ON" : "OFF"); break;
            case 5: snprintf(v, sizeof(v), "%s", sl.reverse ? "ON" : "OFF"); break;
            case 6: snprintf(v, sizeof(v), "%s", sl.load_mono ? "ON (~36s)" : "OFF (~18s)"); break;
        }
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
    if (s_msg[0]){ _fg = TFT_LIGHTGREY; TFT_print(s_msg, 8, _height - fh - 2); }
}

static void cycle_target(int dir){
    int n = sl.slice_target;   // Auto(0) -> 8 -> 16 -> 32 -> 64 -> 128 -> Auto
    if (dir > 0) { if(n == 0) n = 8; else if(n >= 128) n = 0; else n *= 2; }
    else         { if(n == 0) n = 128; else if(n <= 8) n = 0; else n /= 2; }
    sl.slice_target = n;
    sl.ot_active = false;      // dialing a count = leaving OT mode
    slicer_reslice();
}

static void cycle_mode(int dir){
    // Grid -> Transient -> OT (offered only when a .ot sidecar was found)
    int m = sl.ot_active ? 2 : (sl.transient_mode ? 1 : 0);
    int nmodes = sl.ot_present ? 3 : 2;
    m = (m + (dir > 0 ? 1 : nmodes - 1)) % nmodes;
    sl.ot_active = (m == 2);
    sl.transient_mode = (m == 1);
    slicer_reslice();
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
                if(pos==0)      cycle_mode(+1);
                else if(pos==1) cycle_target(+1);
                else if(pos==4) sl.auto_on = !sl.auto_on;
                else if(pos==5) sl.reverse = !sl.reverse;
                else if(pos==6) sl.load_mono = !sl.load_mono;   // applies on next load
            } else { pos++; if(pos >= SL_SETUP_N) pos = -1; }
            setup_redraw(pos, sel);
            break;
        case EV_BWD:
            if(sel){
                if(pos==0)      cycle_mode(-1);
                else if(pos==1) cycle_target(-1);
                else if(pos==4) sl.auto_on = !sl.auto_on;
                else if(pos==5) sl.reverse = !sl.reverse;
                else if(pos==6) sl.load_mono = !sl.load_mono;
            } else { pos--; if(pos < -1) pos = SL_SETUP_N - 1; }
            setup_redraw(pos, sel);
            break;
        case EV_SHORT_PRESS:
            if(pos == -1) return M_MORE;          // System affordance
            if(pos == 2) return M_SLICER_SENS;   // open the dial-in screen
            if(pos == 3){
                setup_refresh_samples();
                return M_SLICER_LOAD;            // open the sample browser
            }
            sel = !sel; s_msg[0] = 0; setup_redraw(pos, sel);
            break;
        case EV_LONG_PRESS: return M_SLICER_LIVE;   // toggle Setup -> Live
        default: break;
    }
    return 0;
}

// ---- Load browser (center-justified sample selector) ----------------------
static void load_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; _fg = TFT_LIGHTGREY;
    char h[32];
    snprintf(h, sizeof(h), "Load Sample  (%d/%d)", s_n_samples ? s_sample_idx + 1 : 0, s_n_samples);
    TFT_print(h, _width / 2 - TFT_getStringWidth(h) / 2, 4);
    if (!s_n_samples){
        char *m = "no samples in usr/";
        _fg = TFT_LIGHTGREY;
        TFT_print(m, _width / 2 - TFT_getStringWidth(m) / 2, _height / 2);
        return;
    }
    int cy = _height / 2;
    // selected sample in a large font, centered
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    int bigfh = TFT_getfontheight();
    _fg = TFT_WHITE;
    char *selnm = s_samples[s_sample_idx];
    TFT_print(selnm, _width / 2 - TFT_getStringWidth(selnm) / 2, cy - bigfh / 2);
    cfont = f;   // back to default for the dim neighbours
    _fg = (color_t){110, 110, 110};
    for (int k = 1; k <= 4; k++){
        int up = s_sample_idx - k, dn = s_sample_idx + k;
        int yup = cy - bigfh / 2 - k * (fh + 4) - 4;
        int ydn = cy + bigfh / 2 + (k - 1) * (fh + 4) + 6;
        if (up >= 0){ char *n = s_samples[up]; TFT_print(n, _width / 2 - TFT_getStringWidth(n) / 2, yup); }
        if (dn < s_n_samples){ char *n = s_samples[dn]; TFT_print(n, _width / 2 - TFT_getStringWidth(n) / 2, ydn); }
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    char *hint = "turn:browse  press:load  hold:cancel";
    TFT_print(hint, _width / 2 - TFT_getStringWidth(hint) / 2, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int slicer_load_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: setup_refresh_samples(); load_redraw(); break;
        case EV_FWD: if(s_n_samples){ s_sample_idx = (s_sample_idx + 1) % s_n_samples; load_redraw(); } break;
        case EV_BWD: if(s_n_samples){ s_sample_idx = (s_sample_idx + s_n_samples - 1) % s_n_samples; load_redraw(); } break;
        case EV_SHORT_PRESS:
            if(s_n_samples) slicer_load(s_samples[s_sample_idx]);
            return M_SLICER_SETUP;
        case EV_LONG_PRESS:
            return M_SLICER_SETUP;   // cancel without loading
        default: break;
    }
    return 0;
}

// ---- Sensitivity dial-in screen -------------------------------------------
// waveform + live slice grid; turning the encoder re-slices so you watch the
// slices appear/disappear as you set the threshold. Forces transient mode.
// waveform + slice lines (only region that changes on adjust)
static void sens_draw_wave(void){
    _bg = BG;
    TFT_fillRect(WX - 2, WY - 2, WW + 4, WH + 4, _bg);
    if (sl.len == 0){ _fg = TFT_LIGHTGREY; TFT_print("no sample", WX + 10, WY + WH / 2); return; }
    int cy = WY + WH / 2;
    for (int c = 0; c < sl.peak_n; c++){
        int x = WX + c * WW / (sl.peak_n ? sl.peak_n : 1);
        int h = sl.peaks[c] * (WH / 2) / 31;
        if (h < 1 && sl.peaks[c] > 0) h = 1;
        TFT_drawLine(x, cy - h, x, cy + h, WAVE);
    }
    for (int s = 0; s <= sl.n_slices; s++){
        int x = slice_x(s);
        TFT_drawLine(x, WY, x, WY + WH, SEL_COL);
    }
}

static void sens_draw_info(void){
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; TFT_fillRect(0, INFO_Y, _width, fh + 4, _bg);
    _fg = TFT_WHITE;
    char s[48];
    snprintf(s, sizeof(s), "Sensitivity  %d      slices: %d", sl.sensitivity, sl.n_slices);
    TFT_print(s, 6, INFO_Y + 2);
}

static void sens_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    sens_draw_wave();
    sens_draw_info();
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn:adjust  hold:done", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int slicer_sens_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU:
            // sensitivity dials the detected COUNT, which only varies in Auto —
            // so force transient + Auto here
            sl.transient_mode = true;
            sl.slice_target = 0;
            slicer_reslice();
            sens_full_redraw();
            break;
        case EV_FWD:
            if(sl.sensitivity < 100){ sl.sensitivity += 5; slicer_reslice(); sens_draw_wave(); sens_draw_info(); }
            break;
        case EV_BWD:
            if(sl.sensitivity > 0){ sl.sensitivity -= 5; slicer_reslice(); sens_draw_wave(); sens_draw_info(); }
            break;
        case EV_LONG_PRESS:
        case EV_SHORT_PRESS:
            return M_SLICER_SETUP;
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
    menusys_new_item(_ms, M_SLICER_LOAD);
    menusys_item_set_default_cb(_ms, M_SLICER_LOAD, slicer_load_handler);
    menusys_new_item(_ms, M_SLICER_SENS);
    menusys_item_set_default_cb(_ms, M_SLICER_SENS, slicer_sens_handler);
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

extern const httpd_uri_t slicer_web_uris[];

const machine_ui_t slicer_menu_ui = {
    .main_items = slicer_main_items,
    .main_targets = slicer_main_targets,
    .n_main = 2,
    .register_pages = slicer_register_pages,
    .main_event = slicer_main_event,
    .boot_target = M_SLICER_LIVE,
    .web_uris = slicer_web_uris,
    .n_web_uris = 1,
};
