// Synth UI — Live (note + gate + knob readout) and Setup (shape / tune / ADSR).
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "sample_browser.h"
#include "synth_priv.h"

static const color_t GATE_ON  = {40, 200, 90};   // note stays this green (gate flips too fast to read)

static const char *const NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static void note_name(float freq, char *buf, size_t n)
{
    if (freq < 1.0f) { snprintf(buf, n, "--"); return; }
    int midi = (int)lroundf(69.0f + 12.0f * log2f(freq / 440.0f));
    if (midi < 0) midi = 0;
    if (midi > 127) midi = 127;
    snprintf(buf, n, "%s%d", NOTE_NAMES[midi % 12], midi / 12 - 1);
}

static bool s_last_gate;
static int  s_last_note = -9999;
static unsigned s_sig_dials = 0, s_sig_adsr = 0;

static int cur_midi(void)
{
    return (sy.freq < 1.0f) ? -1 : (int)lroundf(69.0f + 12.0f * log2f(sy.freq / 440.0f));
}

// ---- compact header: title + engine tag + note name (colour = gate) --------
static void draw_header(void)
{
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; TFT_fillRect(0, 0, _width, fh + 12, _bg);
    _fg = TFT_WHITE; TFT_print("Synth", 6, 4);
    const char *eng = sy.engine == ENG_FM ? "FM" : sy.engine == ENG_WT ? "WT" : "VA";
    _fg = (sy.engine == ENG_WT && !sy.wave_name[0]) ? (color_t){220,80,60} : (color_t){130,130,140};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print((char*)eng, 62, 6);
    TFT_setFont(DEFAULT_FONT, NULL);
    // note name stays green — the gate flips too fast to read as a colour change
    char nm[12]; note_name(sy.freq, nm, sizeof(nm));
    Font f = cfont; TFT_setFont(DEJAVU18_FONT, NULL);
    _fg = GATE_ON; TFT_print(nm, _width - 10 - TFT_getStringWidth(nm), 2);
    cfont = f;
    s_last_gate = sy.gate;
}

// ---- a knob dial: ring + pointer, label + value below ----------------------
static void dial(int cx, int cy, int r, float v01, const char *lab, const char *val, bool live)
{
    int fh = TFT_getfontheight();
    if (v01 < 0) v01 = 0;
    if (v01 > 1) v01 = 1;
    _bg = TFT_BLACK;
    TFT_fillRect(cx - r - 5, cy - r - 2, (r + 5) * 2, r * 2 + 2 * fh + 8, _bg);
    // a knob that hasn't taken over yet (dev unit / untouched) reads dim
    color_t ring = live ? (color_t){0,150,220} : (color_t){70,70,80};
    TFT_drawCircle(cx, cy, r, ring);
    float ang = (-135.0f + v01 * 270.0f) * 0.01745329f;   // deg->rad, 0 = 12 o'clock
    int px = cx + (int)(sinf(ang) * (float)(r - 4));
    int py = cy - (int)(cosf(ang) * (float)(r - 4));
    TFT_drawLine(cx, cy, px, py, live ? (color_t){255,255,255} : (color_t){150,150,160});
    TFT_fillCircle(cx, cy, 3, ring);
    _fg = (color_t){130,130,140};
    TFT_print((char*)lab, cx - TFT_getStringWidth((char*)lab)/2, cy + r + 3);
    _fg = TFT_WHITE;
    TFT_print((char*)val, cx - TFT_getStringWidth((char*)val)/2, cy + r + 3 + fh + 1);
}

// K5 is engine-specific — report its label / value / normalized position
static void k5_desc(const char **lab, char *val, size_t vn, float *v01)
{
    if (sy.engine == ENG_FM)      { *lab = "fm idx"; *v01 = sy.fm_index / 8.0f; snprintf(val, vn, "%.1f", sy.fm_index); }
    else if (sy.engine == ENG_WT) { *lab = "fold";   *v01 = sy.fold;            snprintf(val, vn, "%.0f%%", sy.fold * 100.0f); }
    else                          { *lab = "shape";  *v01 = sy.shape;           snprintf(val, vn, "%.0f%%", sy.shape * 100.0f); }
}

// four macro dials: K5 timbre / K6 cutoff / K7 resonance / K8 env>cut
static void draw_dials(void)
{
    int fh = TFT_getfontheight();
    int cy = fh + 16 + 26, r = 24;
    int cx[4] = { 44, 128, 212, 292 };
    // K5 timbre (engine-aware)
    const char *l0; char v0[16]; float n0;
    k5_desc(&l0, v0, sizeof(v0), &n0);
    dial(cx[0], cy, r, n0, l0, v0, sy.knob_live[0]);
    // K6 cutoff (invert the log map -> 0..1)
    float cv = logf(sy.cutoff_base / 10.0f) / logf(600.0f);
    char cval[16];
    if (sy.cutoff_base >= 1000.0f) snprintf(cval, sizeof(cval), "%.1fk", sy.cutoff_base / 1000.0f);
    else                           snprintf(cval, sizeof(cval), "%.0f", sy.cutoff_base);
    dial(cx[1], cy, r, cv, "cut", cval, sy.knob_live[1]);
    // K7 resonance
    char rval[16]; snprintf(rval, sizeof(rval), "%.0f%%", sy.res01 * 100.0f);
    dial(cx[2], cy, r, sy.res01, "res", rval, sy.knob_live[2]);
    // K8 env>cut
    char eval[16]; snprintf(eval, sizeof(eval), "%.0f%%", sy.env_to_cut * 100.0f);
    dial(cx[3], cy, r, sy.env_to_cut, "env>f", eval, sy.knob_live[3]);
}

// ---- ADSR envelope shape (a polyline you can read at a glance) --------------
static void draw_adsr(void)
{
    int fh = TFT_getfontheight();
    int x = 8, y = fh + 16 + 104, w = _width - 16, h = 44;   // extra gap below the dials
    _bg = TFT_BLACK; TFT_fillRect(x, y - fh - 2, w, h + fh + 4, _bg);
    _fg = (color_t){110,110,120}; TFT_print("ENV", x, y - fh - 2);
    TFT_drawRect(x, y, w, h, (color_t){40, 60, 90});
    float ta = sy.atk, td = sy.dec, tr = sy.rel, tsum = ta + td + tr;
    if (tsum < 1e-4f) tsum = 1e-4f;
    float body = (float)(w - 4) * 0.72f;                  // A+D+R share 72%, sustain plateau the rest
    int aw = (int)(body * ta / tsum), dw = (int)(body * td / tsum), rw = (int)(body * tr / tsum);
    int sw = (w - 4) - aw - dw - rw;
    int xb = x + 2, yb = y + h - 2, yt = y + 2;
    int ys = yb - (int)(sy.sus * (float)(h - 4));
    color_t col = {60, 200, 120};
    int x1 = xb + aw, x2 = x1 + dw, x3 = x2 + sw, x4 = x3 + rw;
    TFT_drawLine(xb, yb, x1, yt, col);
    TFT_drawLine(x1, yt, x2, ys, col);
    TFT_drawLine(x2, ys, x3, ys, col);
    TFT_drawLine(x3, ys, x4, yb, col);
}

static unsigned dials_sig(void)
{
    float k5 = sy.engine == ENG_FM ? sy.fm_index : sy.engine == ENG_WT ? sy.fold : sy.shape;
    return (unsigned)(sy.cutoff_base) * 131u
         + (unsigned)(k5 * 1000.0f) * 17u
         + (unsigned)(sy.res01 * 1000.0f) * 29u
         + (unsigned)(sy.env_to_cut * 1000.0f) * 41u
         + (unsigned)sy.engine * 7u
         + (sy.knob_live[0]?1u:0u) + (sy.knob_live[1]?2u:0u)
         + (sy.knob_live[2]?4u:0u) + (sy.knob_live[3]?8u:0u);
}
static unsigned adsr_sig(void)
{
    return (unsigned)(sy.atk*1000)*7u + (unsigned)(sy.dec*1000)*13u
         + (unsigned)(sy.sus*1000)*17u + (unsigned)(sy.rel*1000)*19u;
}

static void live_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    draw_header();      s_last_note = cur_midi();
    draw_dials();       s_sig_dials = dials_sig();
    draw_adsr();        s_sig_adsr  = adsr_sig();
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("CV1:pitch  TR1:gate   K5:timbre K6:cut K7:res K8:env>f", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int synth_live_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST: {
            // each block redraws only when its own values change (no free-running meter)
            int midi = cur_midi();
            if (midi != s_last_note) { draw_header(); s_last_note = midi; }   // gate no longer recolors
            unsigned ds = dials_sig();
            if (ds != s_sig_dials) { draw_dials(); s_sig_dials = ds; }
            unsigned as = adsr_sig();
            if (as != s_sig_adsr) { draw_adsr(); s_sig_adsr = as; }
            break;
        }
        case EV_LONG_PRESS: return M_SYNTH_SETUP;
        default: break;
    }
    return 0;
}

// ---- Setup -----------------------------------------------------------------
static const char *setup_labels[] = {
    "Engine", "Base Note", "Quantize", "Shape", "FM Ratio", "FM Index",
    "Attack", "Decay", "Sustain", "Release", "Env>Cut", "Glide",
    "LFO Rate", "LFO Depth", "LFO Dest", "Level", "Reverb", "Rev Mix", "Load Wave"
};
#define SY_SETUP_N 19
#define SY_LOAD_ITEM 18          // the "Load Wave" action row

static void setup_val(int i, char *v, size_t n)
{
    switch (i) {
        case 0: snprintf(v, n, "%s", sy.engine == ENG_FM ? "FM" : sy.engine == ENG_WT ? "WT" : "VA"); break;
        case 1: { char nm[12]; note_name(440.0f * powf(2.0f, (sy.base_note - 69) / 12.0f), nm, sizeof(nm));
                  snprintf(v, n, "%s (%d)", nm, sy.base_note); break; }
        case 2: snprintf(v, n, "%s", sy.quantize ? "ON" : "OFF"); break;
        case 3: snprintf(v, n, "%.0f%%", sy.shape * 100.0f); break;
        case 4: snprintf(v, n, "%.2f", sy.fm_ratio); break;
        case 5: snprintf(v, n, "%.1f", sy.fm_index); break;
        case 6: snprintf(v, n, "%d ms", (int)(sy.atk * 1000.0f)); break;
        case 7: snprintf(v, n, "%d ms", (int)(sy.dec * 1000.0f)); break;
        case 8: snprintf(v, n, "%.0f%%", sy.sus * 100.0f); break;
        case 9: snprintf(v, n, "%d ms", (int)(sy.rel * 1000.0f)); break;
        case 10: snprintf(v, n, "%.0f%%", sy.env_to_cut * 100.0f); break;
        case 11: snprintf(v, n, "%d ms", (int)(sy.glide * 1000.0f)); break;
        case 12: snprintf(v, n, "%.1f Hz", sy.lfo_rate); break;
        case 13: snprintf(v, n, "%.0f%%", sy.lfo_depth * 100.0f); break;
        case 14: snprintf(v, n, "%s", sy.lfo_dest == LFO_CUT ? "cutoff" : sy.lfo_dest == LFO_PITCH ? "pitch" : "off"); break;
        case 15: snprintf(v, n, "%.0f%%", sy.level * 100.0f); break;
        case 16: snprintf(v, n, "%s", reverb_mode_name(sy.rv.mode)); break;
        case 17: snprintf(v, n, "%.0f%%", sy.rv.wet * 100.0f); break;
        case 18: snprintf(v, n, "%s", sy.wave_name[0] ? sy.wave_name : "(none)"); break;
    }
}

static void setup_redraw(int pos, int sel)
{
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE; TFT_print("Synth Setup", 6, 4);
    menuTFTPrintAffordance("Machine", pos == -1);
    // scrollable list — the param count exceeds the screen; keep the cursor in view
    int row_h = fh + 5, y0 = fh + 14;
    int vis = (_height - fh - 6 - y0) / row_h;
    if (vis < 1) vis = 1;
    int top = 0;
    if (pos >= vis) top = pos - vis + 1;
    for (int r = 0; r < vis; r++) {
        int i = top + r;
        if (i >= SY_SETUP_N) break;
        int y = y0 + r * row_h;
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 4, _bg);
        TFT_print((char *)setup_labels[i], 8, y);
        char v[24]; setup_val(i, v, sizeof(v));
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("VA/FM; TR1 gates the ADSR; turn to scroll", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void sy_adj(int i, int dir)
{
    float d = (float)dir;
    switch (i) {
        case 0: sy.engine += dir; if (sy.engine < 0) sy.engine = ENG_WT; if (sy.engine > ENG_WT) sy.engine = ENG_VA; break;
        case 1: sy.base_note += dir; if (sy.base_note < 12) sy.base_note = 12; if (sy.base_note > 96) sy.base_note = 96; break;
        case 2: sy.quantize = !sy.quantize; break;
        case 3: sy.shape += d * 0.05f; if (sy.shape < 0) sy.shape = 0; if (sy.shape > 1) sy.shape = 1; break;
        case 4: sy.fm_ratio += d * 0.25f; if (sy.fm_ratio < 0.25f) sy.fm_ratio = 0.25f; if (sy.fm_ratio > 16) sy.fm_ratio = 16; break;
        case 5: sy.fm_index += d * 0.25f; if (sy.fm_index < 0) sy.fm_index = 0; if (sy.fm_index > 12) sy.fm_index = 12; break;
        case 6: sy.atk += d * 0.005f; if (sy.atk < 0.0005f) sy.atk = 0.0005f; if (sy.atk > 2) sy.atk = 2; break;
        case 7: sy.dec += d * 0.01f;  if (sy.dec < 0.001f) sy.dec = 0.001f; if (sy.dec > 2) sy.dec = 2; break;
        case 8: sy.sus += d * 0.05f;  if (sy.sus < 0) sy.sus = 0; if (sy.sus > 1) sy.sus = 1; break;
        case 9: sy.rel += d * 0.02f;  if (sy.rel < 0.001f) sy.rel = 0.001f; if (sy.rel > 3) sy.rel = 3; break;
        case 10: sy.env_to_cut += d * 0.05f; if (sy.env_to_cut < 0) sy.env_to_cut = 0; if (sy.env_to_cut > 1) sy.env_to_cut = 1; break;
        case 11: sy.glide += d * 0.02f; if (sy.glide < 0) sy.glide = 0; if (sy.glide > 2) sy.glide = 2; break;
        case 12: sy.lfo_rate += d * 0.25f; if (sy.lfo_rate < 0.05f) sy.lfo_rate = 0.05f; if (sy.lfo_rate > 20) sy.lfo_rate = 20; break;
        case 13: sy.lfo_depth += d * 0.05f; if (sy.lfo_depth < 0) sy.lfo_depth = 0; if (sy.lfo_depth > 1) sy.lfo_depth = 1; break;
        case 14: sy.lfo_dest += dir; if (sy.lfo_dest < 0) sy.lfo_dest = 2; if (sy.lfo_dest > 2) sy.lfo_dest = 0; break;
        case 15: sy.level += d * 0.05f; if (sy.level < 0) sy.level = 0; if (sy.level > 1) sy.level = 1; break;
        case 16: {   // reverb mode (lazy-init the tank on first non-off)
            int m = sy.rv.mode + dir;
            if (m < 0) m = RV_N_MODES - 1;
            if (m >= RV_N_MODES) m = RV_OFF;
            if (m != RV_OFF && !sy.rv.slab && reverb_init(&sy.rv) != ESP_OK) m = RV_OFF;
            reverb_set_mode(&sy.rv, m);
            break;
        }
        case 17: {
            float w = sy.rv.wet + d * 0.05f;
            if (w < 0) w = 0;
            if (w > 1) w = 1;
            reverb_set_mix(&sy.rv, w);
            break;
        }
    }
}

static int synth_setup_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    static int pos = 0, sel = 0;
    switch (event) {
        case EV_ENTERED_MENU: pos = 0; sel = 0; setup_redraw(pos, sel); break;
        case EV_FWD:
            if (sel) sy_adj(pos, +1);
            else { pos++; if (pos >= SY_SETUP_N) pos = -1; }
            setup_redraw(pos, sel);
            break;
        case EV_BWD:
            if (sel) sy_adj(pos, -1);
            else { pos--; if (pos < -1) pos = SY_SETUP_N - 1; }
            setup_redraw(pos, sel);
            break;
        case EV_SHORT_PRESS:
            if (pos == -1) return M_MORE;
            if (pos == SY_LOAD_ITEM) return M_SYNTH_LOAD;   // -> wave browser
            sel = !sel; setup_redraw(pos, sel);
            break;
        case EV_LONG_PRESS: return M_SYNTH_LIVE;
        default: break;
    }
    return 0;
}

// ---- Load Wave: the shared sample browser -> wavetable ----------------------
static int synth_load_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) {
        sample_browser_enter(false, "Load Wave", sy.wave_name);   // alphabetical
        return 0;
    }
    int r = sample_browser_event(event);
    if (r == 1) {                                    // picked
        const char *sel = sample_browser_selected();
        if (synth_load_wave(sel) == 0) sy.engine = ENG_WT;   // loading a wave selects WT
        return M_SYNTH_SETUP;
    }
    if (r == 2) return M_SYNTH_SETUP;                // cancelled
    return 0;
}

// ---- CV Matrix page: per-destination source + bipolar amount ---------------
static const char *const mtx_labels[SYM_N] = {
    "Cutoff", "Reso", "Timbre", "Env>Cut", "LFO Rate", "LFO Dep", "Level", "Pitch"
};

static void mtx_redraw(int pos, int field)   // field: 0 nav / 1 edit src / 2 edit amt
{
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE; TFT_print("Synth CV Matrix", 6, 4);
    menuTFTPrintAffordance("Setup", pos == -1);
    int row_h = fh + 5, y0 = fh + 14;
    int vis = (_height - fh - 6 - y0) / row_h;
    if (vis < 1) vis = 1;
    int top = 0;
    if (pos >= vis) top = pos - vis + 1;
    for (int r = 0; r < vis; r++) {
        int i = top + r;
        if (i >= SYM_N) break;
        int y = y0 + r * row_h;
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        TFT_fillRect(0, y - 2, _width, fh + 4, _bg);
        _fg = TFT_WHITE;
        TFT_print((char *)mtx_labels[i], 8, y);
        char src[8];
        if (sy.mtx_src[i] < 0) snprintf(src, sizeof(src), "off");
        else                   snprintf(src, sizeof(src), "CV%d", sy.mtx_src[i] + 1);
        char amt[10]; snprintf(amt, sizeof(amt), "%+d%%", (int)(sy.mtx_amt[i] * 100.0f));
        _fg = (i == pos && field == 1) ? TFT_CYAN : (color_t){170,170,180};
        TFT_print(src, _width - 128, y);
        _fg = (sy.mtx_src[i] < 0) ? (color_t){80,80,80}
            : (i == pos && field == 2) ? TFT_CYAN : TFT_WHITE;
        TFT_print(amt, _width - TFT_getStringWidth(amt) - 10, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("press: src > amt > done   turn: change", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int synth_matrix_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    static int pos = 0, field = 0;
    switch (event) {
        case EV_ENTERED_MENU: pos = 0; field = 0; mtx_redraw(pos, field); break;
        case EV_FWD:
            if (field == 1)      { int s = sy.mtx_src[pos] + 1; if (s > 7)  s = -1; sy.mtx_src[pos] = (int8_t)s; }
            else if (field == 2) { float a = sy.mtx_amt[pos] + 0.05f; if (a > 1.0f) a = 1.0f; sy.mtx_amt[pos] = a; }
            else { pos++; if (pos >= SYM_N) pos = -1; }
            mtx_redraw(pos, field);
            break;
        case EV_BWD:
            if (field == 1)      { int s = sy.mtx_src[pos] - 1; if (s < -1) s = 7; sy.mtx_src[pos] = (int8_t)s; }
            else if (field == 2) { float a = sy.mtx_amt[pos] - 0.05f; if (a < -1.0f) a = -1.0f; sy.mtx_amt[pos] = a; }
            else { pos--; if (pos < -1) pos = SYM_N - 1; }
            mtx_redraw(pos, field);
            break;
        case EV_SHORT_PRESS:
            if (pos == -1) return M_SYNTH_SETUP;
            field = (field + 1) % 3;                     // nav -> src -> amt -> nav
            mtx_redraw(pos, field);
            break;
        case EV_LONG_PRESS: return M_SYNTH_LIVE;
        default: break;
    }
    return 0;
}

static void synth_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_SYNTH_LIVE);   menusys_item_set_default_cb(_ms, M_SYNTH_LIVE, synth_live_handler);
    menusys_new_item(_ms, M_SYNTH_SETUP);  menusys_item_set_default_cb(_ms, M_SYNTH_SETUP, synth_setup_handler);
    menusys_new_item(_ms, M_SYNTH_LOAD);   menusys_item_set_default_cb(_ms, M_SYNTH_LOAD, synth_load_handler);
    menusys_new_item(_ms, M_SYNTH_MATRIX); menusys_item_set_default_cb(_ms, M_SYNTH_MATRIX, synth_matrix_handler);
}

static int synth_main_event(int event, void *ev_data)
{
    (void)ev_data;
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW) {
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        char nm[12]; note_name(sy.freq, nm, sizeof(nm));
        _fg = sy.gate ? GATE_ON : TFT_LIGHTGREY;
        char s[40]; snprintf(s, sizeof(s), "Synth: %s %s", nm, sy.gate ? "GATE" : "");
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const synth_main_items[] = { "Live", "Setup", "Matrix" };
static const int synth_main_targets[] = { M_SYNTH_LIVE, M_SYNTH_SETUP, M_SYNTH_MATRIX };

const machine_ui_t synth_menu_ui = {
    .main_items = synth_main_items,
    .main_targets = synth_main_targets,
    .n_main = 3,
    .register_pages = synth_register_pages,
    .main_event = synth_main_event,
    .boot_target = M_SYNTH_LIVE,
};
