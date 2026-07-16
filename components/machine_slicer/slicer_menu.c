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
#include "sample_browser.h"
#include "setup_menu.h"
#include "slicer_priv.h"

static const color_t BG      = {0, 0, 0};        // black canvas (deck grammar)
static const color_t WAVE    = {70, 110, 180};
static const color_t GRID    = {40, 60, 110};
static const color_t SEL_COL = {70, 235, 90};    // selected slice (green)
static const color_t CUR_COL = {40, 200, 90};    // playing slice

// waveform box — near the top now; sample/grid info sits below it
// three selectable screen elements, each a box (deck/sampler grammar):
//   transport (waveform)  |  file name (big, under it)  |  FX box
#define EB_X 6
#define EB_W (_width - 12)
#define TB_Y 6                     // transport box top
#define WH   80                    // waveform height (inside the box)
#define TB_H (WH + 12)             // transport box height (6 px margin for a thick border)
#define WX  (EB_X + 6)             // waveform inside the transport box
#define WY  (TB_Y + 6)
#define WW  (EB_W - 12)
#define FB_Y (TB_Y + TB_H + 5)     // file-name box
#define FB_H 42
#define XB_Y (FB_Y + FB_H + 5)     // FX box
#define XB_H 54

static int s_last_cur = -1, s_last_sel = -1, s_last_slices = -1, s_last_peaks = -1;
static bool s_last_loading = true;   // reader-load in progress at the last redraw
// two-level encoder: level 1 picks an element (0=Bar 1=File 2=FX); pressing Bar
// enters level 2 (s_in_bar) where turns hand-select slices and press fires.
static int  s_elem = 0;
static bool s_in_bar = false;
static void draw_speed(void);   // fwd: CV7 speed indicator, defined near draw_waveform

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
    color_t wcol = (s == sel) ? SEL_COL : WAVE;    // selected slice's waveform is green
    for (int c = 0; c < sl.peak_n; c++){
        int x = WX + c * WW / sl.peak_n;
        if (x < x0 || x >= x1) continue;
        int h = sl.peaks[c] * (WH / 2) / 31;
        if (h < 1 && sl.peaks[c] > 0) h = 1;
        TFT_drawLine(x, cy - h, x, cy + h, wcol);
    }
    TFT_drawLine(x0, cy, x1, cy, TFT_WHITE);        // white center line (this column)
    TFT_drawLine(x0, WY, x0, WY + WH, GRID);
    TFT_drawLine(x1, WY, x1, WY + WH, GRID);
    if (s == sel) TFT_drawRect(x0, WY, x1 - x0, WH, SEL_COL);
    if (is_cur)   TFT_drawRect(x0, WY, x1 - x0, WH, CUR_COL);
}

// sample name + grid size + live slice number, below the waveform
// slice count on the right of the file box (cheap; updated on slice moves)
static void draw_slice_count(void){
    Font f = cfont; TFT_setFont(DEFAULT_FONT, NULL);
    int fh = TFT_getfontheight();
    char sc[16]; snprintf(sc, sizeof(sc), "%d/%d", sl.n_slices ? sl.sel + 1 : 0, sl.n_slices);
    _bg = TFT_BLACK; TFT_fillRect(EB_X + EB_W - 78, FB_Y + FB_H/2 - fh/2 - 2, 74, fh + 4, _bg);
    _fg = (color_t){120, 160, 210};
    TFT_print(sc, EB_X + EB_W - TFT_getStringWidth(sc) - 12, FB_Y + FB_H/2 - fh/2);
    cfont = f;
}

// file-name box: big sample name + slice count
static void draw_file_box(void){
    _bg = TFT_BLACK; TFT_fillRect(EB_X + 1, FB_Y + 1, EB_W - 2, FB_H - 2, _bg);
    Font f = cfont; TFT_setFont(DEJAVU24_FONT, NULL);
    _fg = sl.sample[0] ? TFT_WHITE : TFT_LIGHTGREY;
    char nm[28]; snprintf(nm, sizeof(nm), "%s", sl.sample[0] ? sl.sample : "(none)");
    TFT_print(nm, EB_X + 10, FB_Y + FB_H/2 - TFT_getfontheight()/2);
    cfont = f;
    draw_slice_count();
}

// FX box (drums filter-box style): FX on/off + filter + reverb readout
static void draw_fx_box(void){
    _bg = TFT_BLACK; TFT_fillRect(EB_X + 1, XB_Y + 1, EB_W - 2, XB_H - 2, _bg);
    int fh = TFT_getfontheight();
    _fg = sl.fx_on ? (color_t){60, 200, 120} : (color_t){110, 110, 120};
    char s[16]; snprintf(s, sizeof(s), "FX  %s", sl.fx_on ? "ON" : "off");
    TFT_print(s, EB_X + 10, XB_Y + 7);
    _fg = sl.fx_on ? (color_t){140, 160, 200} : (color_t){80, 80, 90};
    char t[52];
    snprintf(t, sizeof(t), "cut %.0f  res %.0f%%   %s %.0f%%",
             sl.fx_cut, sl.fx_res * 100.0f, reverb_mode_name(sl.fx_rv.mode), sl.fx_rvmix * 100.0f);
    TFT_print(t, EB_X + 10, XB_Y + 7 + fh + 5);
}

// a box border: 3 px THICK when selected, thin when not (inner 2 px cleared to
// black on deselect so a previous thick border leaves no ghost — box interiors
// are black / the waveform starts 3 px in)
// draw a box border `thick` px in `c`. Always clears the 4 px border band to
// black first, so a border can shrink (level 2 -> level 1) with no ghost. Every
// box margin is >= 5 px, so this never touches content.
static void box_border(int x, int y, int w, int h, color_t c, int thick){
    for (int i = 1; i <= 4; i++) TFT_drawRect(x + i, y + i, w - 2 * i, h - 2 * i, (color_t){0, 0, 0});
    for (int i = 0; i < thick; i++) TFT_drawRect(x + i, y + i, w - 2 * i, h - 2 * i, c);
}

// borders encode the MENU LEVEL: thin (1-2 px) while browsing elements, THICK
// (4 px, green) once the transport is entered (level 2); exit -> thin again.
static void draw_highlights(void){
    color_t sel = TFT_WHITE, dim = {46, 46, 60};
    if (s_in_bar)         box_border(EB_X, TB_Y, EB_W, TB_H, sel, 4);   // transport: level 2 (thick white)
    else if (s_elem == 0) box_border(EB_X, TB_Y, EB_W, TB_H, sel, 2);   // selected (thin white)
    else                  box_border(EB_X, TB_Y, EB_W, TB_H, dim, 1);   // unselected
    box_border(EB_X, FB_Y, EB_W, FB_H, s_elem == 1 ? sel : dim, s_elem == 1 ? 2 : 1);
    box_border(EB_X, XB_Y, EB_W, XB_H, s_elem == 2 ? sel : dim, s_elem == 2 ? 2 : 1);
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
    draw_slice_count();                 // keep the slice number current
    draw_speed();                       // repaint over any slice redraw
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

// CV7 playback-speed indicator (transport top-left): "1:1" green when locked at
// unity, else the base varispeed ratio (e.g. "1.42x"); v/oct is separate
static void draw_speed(void){
    float base;
    if (sl.pitch_cv >= 1843 && sl.pitch_cv <= 2253) base = 1.0f;
    else if (sl.pitch_cv > 2253) base = 1.0f + (float)(sl.pitch_cv - 2253) / 1842.0f;
    else                         base = 0.5f + (float)sl.pitch_cv / 1843.0f * 0.5f;
    bool lock = (base > 0.99f && base < 1.01f);
    char s[10];
    if (lock) snprintf(s, sizeof(s), "1:1");
    else      snprintf(s, sizeof(s), "%.2fx", base);
    Font f = cfont; TFT_setFont(DEFAULT_FONT, NULL);
    int fh = TFT_getfontheight();
    _bg = BG; TFT_fillRect(WX + 1, WY + 1, 54, fh + 2, _bg);
    _fg = lock ? (color_t){60, 220, 90} : (color_t){150, 160, 200};
    TFT_print(s, WX + 3, WY + 1);
    cfont = f;
}

static void draw_waveform(void){
    int cy = WY + WH / 2;
    // current slice block gets a tinted fill first (background layer)
    if (sl.playing) draw_slice_region(sl.cur, CUR_COL, true);
    // peaks
    int sx0 = slice_x(sl.sel), sx1 = slice_x(sl.sel + 1);   // selected slice x-range -> green peaks
    for (int c = 0; c < sl.peak_n; c++){
        int x = WX + c * WW / (sl.peak_n ? sl.peak_n : 1);
        int h = sl.peaks[c] * (WH / 2) / 31;
        if (h < 1 && sl.peaks[c] > 0) h = 1;
        TFT_drawLine(x, cy - h, x, cy + h, (x >= sx0 && x < sx1) ? SEL_COL : WAVE);
    }
    TFT_drawLine(WX, cy, WX + WW - 1, cy, TFT_WHITE);   // white center line
    // slice division grid (boundaries may be non-uniform in transient mode)
    for (int s = 0; s <= sl.n_slices; s++){
        int x = slice_x(s);
        TFT_drawLine(x, WY, x, WY + WH, GRID);
    }
    // selected slice outline (bright) + current playing outline
    draw_slice_region(sl.sel, SEL_COL, false);
    if (sl.playing) draw_slice_region(sl.cur, CUR_COL, false);
    draw_speed();
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
    draw_file_box();
    draw_fx_box();
    draw_highlights();
    s_last_cur = sl.cur; s_last_sel = sl.sel; s_last_slices = sl.n_slices; s_last_peaks = sl.peak_n;
    s_last_loading = sl.loading;
}

static int slicer_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU:
            s_elem = 0; s_in_bar = false; sl.ui_ctx = 0;
            live_full_redraw();
            break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST:
            // redraw when the reader-load finishes (loading true->false) — the
            // definitive "peaks + slices ready" signal, even if the counts didn't
            // change — or if the grid/peak count changed
            if (sl.loading != s_last_loading || sl.n_slices != s_last_slices || sl.peak_n != s_last_peaks)
                live_full_redraw();
            else if (sl.cur != s_last_cur || sl.sel != s_last_sel)
                live_update_highlights();                           // just move highlights
            {   // live FX readout while the filter knobs / reverb settings move
                static unsigned s_fxsig = 0;
                unsigned fxsig = (sl.fx_on ? 1u : 0u) + (unsigned)sl.fx_cut
                               + (unsigned)(sl.fx_res * 1000.0f) * 7u
                               + (unsigned)sl.fx_rv.mode * 131u
                               + (unsigned)(sl.fx_rvmix * 1000.0f) * 17u;
                if (fxsig != s_fxsig){ draw_fx_box(); s_fxsig = fxsig; }
            }
            {   // speed indicator follows knob7 (CV7 varispeed)
                static int s_pcv = -1;
                if (sl.pitch_cv != s_pcv){ draw_speed(); s_pcv = sl.pitch_cv; }
            }
            if (event == EV_TIMER_REPEATING_SLOW){
                // streaming internals through /status v1 (remote debugging)
                char dbg[48];
                snprintf(dbg, sizeof(dbg), "%c c%d n%d S%lu g%lu a%lu",
                         sl.playing ? 'P' : 's', sl.cur, sl.n_slices,
                         (unsigned long)sl.dbg_starve,
                         (unsigned long)sl.gen,
                         (unsigned long)sl.ring_avail);
                audio_status_set_voices("slicer", dbg);
            }
            break;
        case EV_FWD:
            if (s_in_bar){ sl.sel = (sl.sel + 1) % sl.n_slices; live_update_highlights(); }
            else { s_elem = (s_elem + 1) % 3; sl.ui_ctx = (s_elem == 2 && sl.fx_on) ? 1 : 0; draw_highlights(); }
            break;
        case EV_BWD:
            if (s_in_bar){ sl.sel = (sl.sel + sl.n_slices - 1) % sl.n_slices; live_update_highlights(); }
            else { s_elem = (s_elem + 2) % 3; sl.ui_ctx = (s_elem == 2 && sl.fx_on) ? 1 : 0; draw_highlights(); }
            break;
        case EV_SHORT_PRESS:
            if (s_in_bar) sl.cmd_fire = 1;                                    // play the selected slice
            else if (s_elem == 0){ s_in_bar = true; draw_highlights(); }      // enter the bar
            else if (s_elem == 1) return M_SLICER_LOAD;                       // File -> browser (SLICES)
            else { sl.fx_on = !sl.fx_on; sl.ui_ctx = sl.fx_on ? 1 : 0; draw_fx_box(); draw_highlights(); }  // FX toggle (only hijacks knobs when ON)
            break;
        case EV_LONG_PRESS:
            if (s_in_bar){ s_in_bar = false; draw_highlights(); break; }      // pop out of the bar
            sl.ui_ctx = 0;
            return M_SLICER_SETUP;   // element level: Live -> Setup (no hub)
        default: break;
    }
    return 0;
}

// ---- Setup page (shared setup-menu framework) -----------------------------
static const setup_item_t sl_setup_items[] = {
    {"Mode",        ST_TOGGLE},   // Grid / Transient / OT
    {"Slices",      ST_TOGGLE},   // Auto / 8 / 16 / 32 / 64 / 128
    {"Sensitivity", ST_ACTION},   // -> M_SLICER_SENS dial-in
    {"Sample",      ST_ACTION},   // -> M_SLICER_LOAD browser
    {"Auto",        ST_TOGGLE},
    {"Reverse",     ST_TOGGLE},
    {"Reverb",      ST_TOGGLE},   // reverb mode cycle
    {"Rev Mix",     ST_RANGE},    // 0..100%
};

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

// value string for setup item i (extracted from the old setup_redraw switch)
static void sl_setup_val(int i, char *v, size_t n){
    switch(i){
        case 0: snprintf(v, n, "%s", sl.ot_active ? "OT"
                            : (sl.transient_mode ? "Transient" : "Grid")); break;
        case 1:
            if(sl.slice_target == 0) snprintf(v, n, "Auto");
            else                     snprintf(v, n, "%d", sl.slice_target);
            break;
        case 2: snprintf(v, n, "%d", sl.sensitivity); break;
        case 3: snprintf(v, n, "%s", sl.sample[0] ? sl.sample : "(none)"); break;
        case 4: snprintf(v, n, "%s", sl.auto_on ? "ON" : "OFF"); break;
        case 5: snprintf(v, n, "%s", sl.reverse ? "ON" : "OFF"); break;
        case 6: snprintf(v, n, "%s", reverb_mode_name(sl.fx_rv.mode)); break;
        case 7: snprintf(v, n, "%.0f%%", sl.fx_rvmix * 100.0f); break;
        default: v[0] = 0; break;
    }
}

// TOGGLE items cycle; RANGE items step +/- ; ACTION items (2,3) do nothing here
static void sl_setup_adj(int i, int dir){
    switch(i){
        case 0: cycle_mode(dir); break;
        case 1: cycle_target(dir); break;
        case 4: sl.auto_on = !sl.auto_on; break;
        case 5: sl.reverse = !sl.reverse; break;   // reader rebuilds heads
        case 6: {
            int m = sl.fx_rv.mode + (dir > 0 ? 1 : -1);
            if(m >= RV_N_MODES) m = RV_OFF;
            if(m < 0)           m = RV_N_MODES - 1;
            reverb_set_mode(&sl.fx_rv, m);
            break;
        }
        case 7:
            sl.fx_rvmix += (dir > 0 ? 0.05f : -0.05f);
            if(sl.fx_rvmix > 1) sl.fx_rvmix = 1;
            if(sl.fx_rvmix < 0) sl.fx_rvmix = 0;
            reverb_set_mix(&sl.fx_rv, sl.fx_rvmix);
            break;
        default: break;
    }
}

static int sl_setup_action(int i){
    if(i == 2) return M_SLICER_SENS;   // open the dial-in screen
    if(i == 3) return M_SLICER_LOAD;   // open the sample browser
    return 0;
}

static setup_menu_t sl_setup = {
    .items       = sl_setup_items,
    .n           = 8,
    .title       = "Slicer Setup",
    .aff_label   = "Machine",
    .aff_target  = M_MORE,
    .live_target = M_SLICER_LIVE,
    .render      = sl_setup_val,
    .adjust      = sl_setup_adj,
    .action      = sl_setup_action,
};

static int slicer_setup_handler(int it_id, int event, void *ev_data){
    (void)it_id; (void)ev_data;
    return setup_menu_event(&sl_setup, event);
}

// ---- Load browser: the shared two-level widget (folders -> big-name list) --
static int slicer_load_handler(int it_id, int event, void *ev_data){
    if (event == EV_ENTERED_MENU){ sample_browser_enter_dir(false, "Load Sample", sl.sample, SAMPLE_DIR_SLICES); return 0; }
    int r = sample_browser_event(event);
    if (r == 1){ slicer_load((char*)sample_browser_selected()); return M_SLICER_SETUP; }
    if (r == 2) return M_SLICER_SETUP;
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
    int y = WY + WH + 10;
    _bg = TFT_BLACK; TFT_fillRect(0, y, _width, fh + 4, _bg);
    _fg = TFT_WHITE;
    char s[48];
    snprintf(s, sizeof(s), "Sensitivity  %d      slices: %d", sl.sensitivity, sl.n_slices);
    TFT_print(s, 6, y + 2);
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
