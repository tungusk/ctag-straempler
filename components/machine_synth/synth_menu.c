// Synth UI — Live (note + gate + knob readout) and Setup (shape / tune / ADSR).
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "setup_menu.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "sample_browser.h"
#include "clock.h"        // LFO sync readout
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
static unsigned s_sig_dial[4], s_sig_adsr = 0, s_sig_osc = 0;
// Redraw-speed discipline (2026-09-09): every element clears its own rect so
// it can be redrawn alone; a FULL redraw already cleared the screen, so the
// per-element clears are skipped then (they were ~56k px of overdraw on top
// of the 77k px fillScreen). Ticks redraw only the element that changed:
// one dial, the note field, one ADSR strip.
static bool s_skip_clear = false;
#define CLEAR_RECT(x, y, w, h) do { if (!s_skip_clear) TFT_fillRect((x), (y), (w), (h), TFT_BLACK); } while (0)

// two-level encoder nav (mirrors Keys Live): browse elements, click in to edit,
// long-press to escape. Elements: 0=Wave (header tag), 1..4 = dials
// (timbre/cut/res/env>f), 5..8 = ADSR points (A/D/S/R).
// The bottom panel is TWO views sharing one strip: the ADSR graph and an LFO
// section. Its title ("ENV" / "LFO") is the last nav element — press it to
// switch. Element count therefore depends on the view: 0 = tag/osc, 1..4 =
// dials, then the view's own params, then the title.
#define SLIVE_BASE   5                    // tag + four dials
#define SLIVE_ENV_N  4                    // A D S R
#define SLIVE_LFO_N  5                    // sync, div/rate, shape, amount, dest
static int s_env_view = 0;                // 0 = ADSR, 1 = LFO
static int slive_n(void)        { return SLIVE_BASE + 1 + (s_env_view ? SLIVE_LFO_N : SLIVE_ENV_N); }
// the header sits BEFORE its own params, so the scroll reads in order:
// tag, dials, ENV/LFO header, then that view's values (Arlo 2026-09-10)
static int slive_title_el(void) { return SLIVE_BASE; }
static int slive_param0(void)   { return SLIVE_BASE + 1; }
static int  s_live_sel  = 1;      // start on the first dial
static bool s_live_edit = false;
static int slive_focus(int e) { return s_live_sel != e ? 0 : (s_live_edit ? 2 : 1); }
static inline float sclampf(float x, float lo, float hi){ return x < lo ? lo : x > hi ? hi : x; }

static int cur_midi(void)
{
    return (sy.freq < 1.0f) ? -1 : (int)lroundf(69.0f + 12.0f * log2f(sy.freq / 440.0f));
}

// ---- compact header: title + engine tag + note name (colour = gate) --------
// the note name lives in its own field at the right edge of the header so a
// note change repaints ~2k px (was the whole 8k px header + three strings)
#define NOTE_FIELD_W 78
static void draw_note(void)
{
    int fh = TFT_getfontheight();
    char nm[12]; note_name(sy.freq, nm, sizeof(nm));
    _bg = TFT_BLACK;
    CLEAR_RECT(_width - NOTE_FIELD_W, 0, NOTE_FIELD_W, fh + 12);
    Font f = cfont; TFT_setFont(DEJAVU18_FONT, NULL);
    _fg = GATE_ON; TFT_print(nm, _width - 10 - TFT_getStringWidth(nm), 2);
    cfont = f;
    s_last_gate = sy.gate;
    s_last_note = cur_midi();
}

static void draw_header(void)
{
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; CLEAR_RECT(0, 0, _width - NOTE_FIELD_W, fh + 12);
    _fg = TFT_WHITE; TFT_print("Synth", 6, 4);
    const char *eng = sy.engine == ENG_FM ? "FM" : sy.engine == ENG_WT ? "WT" : "VA";
    int hf = slive_focus(0);   // Wave element focus tints the header tag
    if (hf) _fg = hf == 2 ? (color_t){130,255,150} : (color_t){210,190,120};
    else    _fg = (sy.engine == ENG_WT && !sy.wave_name[0]) ? (color_t){220,80,60} : (color_t){130,130,140};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    // show the wavetable name (WT) next to the engine tag, else just the engine
    char htag[28];
    if (sy.wave_name[0]) snprintf(htag, sizeof(htag), "%s %s", eng, sy.wave_name);
    else                 snprintf(htag, sizeof(htag), "%s", eng);
    TFT_print(htag, 62, 6);
    TFT_setFont(DEFAULT_FONT, NULL);
    // note name stays green — the gate flips too fast to read as a colour change
    draw_note();
}

// ---- oscillator preview: one+ cycle of the current voice shape --------------
// VA: saw<->square morph by shape. FM: the ratio/index modulated sine. WT: sine
// placeholder. Element 0 of the encoder nav (press -> Load Wave).
static void draw_osc(void)
{
    int fh = TFT_getfontheight();
    // h was 26 -> amp 11, which quantised the peaks of a high-ratio FM shape
    // onto one pixel row and read as a FLAT, truncated top and bottom. The band
    // now runs from just under the header (clear ends fh+12) to just above the
    // dials' clear (starts fh+50), so amp is 14.
    int x = 8, y0 = fh + 14, w = _width - 16, h = 32, cy = y0 + h / 2;
    _bg = TFT_BLACK; CLEAR_RECT(x - 1, y0 - 2, w + 2, h + 4);
    // encoder-nav focus = a BOLDER trace (2 px, brighter), not a box
    int foc = (s_live_sel == 0);
    color_t wc = foc ? (s_live_edit ? (color_t){130, 255, 150} : (color_t){180, 225, 255})
                     : (color_t){120, 190, 235};
    int prevy = cy, amp = h / 2 - 2, cycles = 2;
    for (int i = 0; i <= w; i++) {
        float ph = (float)i / (float)w * (float)cycles; ph -= (float)(int)ph;
        float yv;
        if (sy.engine == ENG_FM) {
            float m = sinf(6.2831853f * sy.fm_ratio * ph);
            yv = sinf(6.2831853f * ph + sy.fm_index * m);
        } else if (sy.engine == ENG_WT) {
            yv = sinf(6.2831853f * ph);
        } else {                                 // VA saw<->square morph
            float saw = 1.0f - 2.0f * ph, sq = ph < 0.5f ? 1.0f : -1.0f;
            yv = (1.0f - sy.shape) * saw + sy.shape * sq;
        }
        int py = cy - (int)(yv * (float)amp);
        if (i > 0) {
            // one vertical span per column joining the previous sample to this
            // one: ONE SPI transaction per column instead of a per-pixel
            // diagonal line (each pixel was its own address-window + write)
            int a = prevy < py ? prevy : py, b = prevy < py ? py : prevy;
            TFT_drawFastVLine(x + i, a, b - a + 2 + (foc ? 1 : 0), wc);   // 2 px trace, 3 px when focused
        }
        prevy = py;
    }
}

// ---- a knob dial: ring + pointer, label + value below ----------------------
static void dial(int cx, int cy, int r, float v01, const char *lab, const char *val, bool live, int hi)
{
    int fh = TFT_getfontheight();
    if (v01 < 0) v01 = 0;
    if (v01 > 1) v01 = 1;
    _bg = TFT_BLACK;
    // clear generously: the pointer now ESCAPES past the ring (to r+7) and the
    // focus ring sits at r+4 — cover both + the label row so nothing is stranded
    int m = r + 8;
    CLEAR_RECT(cx - m, cy - m, m * 2, m + r + fh + 8);
    if (hi) {                                          // encoder-nav focus ring
        color_t hc = hi == 2 ? (color_t){130, 255, 150} : (color_t){150, 150, 170};
        TFT_drawCircle(cx, cy, r + 3, hc);
        if (hi == 2) TFT_drawCircle(cx, cy, r + 4, hc);
    }
    // a knob that hasn't taken over yet (dev unit / untouched) reads dim
    color_t ring = live ? (color_t){0,150,220} : (color_t){70,70,80};
    TFT_drawCircle(cx, cy, r, ring);
    // pointer that ESCAPES the ring: a radial tick from just inside the rim to
    // past it, leaving the centre free for the value readout
    float ang = (-135.0f + v01 * 270.0f) * 0.01745329f;   // deg->rad, 0 = 12 o'clock
    float sn = sinf(ang), cs = cosf(ang);
    color_t ncol = live ? (color_t){255,255,255} : (color_t){175,175,185};
    TFT_drawLine(cx + (int)(sn * (r - 3)), cy - (int)(cs * (r - 3)),
                 cx + (int)(sn * (r + 7)), cy - (int)(cs * (r + 7)), ncol);
    // value in the MIDDLE of the dial
    _fg = TFT_WHITE;
    TFT_print((char*)val, cx - TFT_getStringWidth((char*)val)/2, cy - fh / 2);
    // label stays BELOW the circle
    _fg = (color_t){130,130,140};
    TFT_print((char*)lab, cx - TFT_getStringWidth((char*)lab)/2, cy + r + 4);
}

// K5 is engine-specific — report its label / value / normalized position
static void k5_desc(const char **lab, char *val, size_t vn, float *v01)
{
    if (sy.engine == ENG_FM)      { *lab = "fm idx"; *v01 = sy.fm_index / 8.0f; snprintf(val, vn, "%.1f", sy.fm_index); }
    else if (sy.engine == ENG_WT) { *lab = "fold";   *v01 = sy.fold;            snprintf(val, vn, "%.0f%%", sy.fold * 100.0f); }
    else                          { *lab = "shape";  *v01 = sy.shape;           snprintf(val, vn, "%.0f%%", sy.shape * 100.0f); }
}

// four macro dials: K5 timbre / K6 cutoff / K7 resonance / K8 env>cut.
// Each is drawn on its own so a knob move repaints ONE dial (~5k px), not four.
static unsigned dial_sig(int i)
{
    switch (i) {
        case 0: { float k5 = sy.engine == ENG_FM ? sy.fm_index : sy.engine == ENG_WT ? sy.fold : sy.shape;
                  return (unsigned)(k5 * 1000.0f) * 17u + (unsigned)sy.engine * 7u + (sy.mtx.live[SYM_TIMBRE] ? 1u : 0u) + (unsigned)slive_focus(1) * 3u; }
        case 1:   return (unsigned)(sy.cutoff_base) * 131u + (sy.mtx.live[SYM_CUTOFF] ? 2u : 0u) + (unsigned)slive_focus(2) * 3u;
        case 2:   return (unsigned)(sy.res01 * 1000.0f) * 29u + (sy.mtx.live[SYM_RES] ? 4u : 0u) + (unsigned)slive_focus(3) * 3u;
        default:  return (unsigned)(sy.env_to_cut * 1000.0f) * 41u + (sy.mtx.live[SYM_ENVCUT] ? 8u : 0u) + (unsigned)slive_focus(4) * 3u;
    }
}
static void draw_dial(int i)
{
    int fh = TFT_getfontheight();
    int cy = fh + 16 + 26 + 40, r = 24;   // +40 (was +32): extra room for the escaping needle below the osc strip
    static const int cx[4] = { 40, 120, 200, 280 };    // recentred + slightly tighter for the escaping-needle dials
    char val[16];
    switch (i) {
        case 0: {                                          // K5 timbre (engine-aware)
            const char *l0; float n0;
            k5_desc(&l0, val, sizeof(val), &n0);
            dial(cx[0], cy, r, n0, l0, val, sy.mtx.live[SYM_TIMBRE], slive_focus(1));
            break;
        }
        case 1: {                                          // K6 cutoff (invert the log map -> 0..1)
            float cv = logf(sy.cutoff_base / 10.0f) / logf(600.0f);
            if (sy.cutoff_base >= 1000.0f) snprintf(val, sizeof(val), "%.1fk", sy.cutoff_base / 1000.0f);
            else                           snprintf(val, sizeof(val), "%.0f", sy.cutoff_base);
            dial(cx[1], cy, r, cv, "cut", val, sy.mtx.live[SYM_CUTOFF], slive_focus(2));
            break;
        }
        case 2:                                            // K7 resonance
            snprintf(val, sizeof(val), "%.0f%%", sy.res01 * 100.0f);
            dial(cx[2], cy, r, sy.res01, "res", val, sy.mtx.live[SYM_RES], slive_focus(3));
            break;
        default:                                           // K8 env>cut
            snprintf(val, sizeof(val), "%.0f%%", sy.env_to_cut * 100.0f);
            dial(cx[3], cy, r, sy.env_to_cut, "env>f", val, sy.mtx.live[SYM_ENVCUT], slive_focus(4));
            break;
    }
    s_sig_dial[i] = dial_sig(i);
}
static void draw_dials(void) { for (int i = 0; i < 4; i++) draw_dial(i); }
// redraw only the dials whose value/focus changed; returns how many
static int draw_dials_changed(void)
{
    int n = 0;
    for (int i = 0; i < 4; i++) if (dial_sig(i) != s_sig_dial[i]) { draw_dial(i); n++; }
    return n;
}

// ---- ADSR envelope shape (a polyline you can read at a glance) --------------
static void draw_adsr(void)
{
    int fh = TFT_getfontheight();
    int x = 8, y = fh + 16 + 104 + 32, w = _width - 16, h = 40;   // +32 for the osc strip
    _bg = TFT_BLACK; CLEAR_RECT(x, y - fh - 2, w, h + fh + 4);
    int tf = (s_live_sel == slive_title_el());
    _fg = tf ? (s_live_edit ? (color_t){130,255,150} : (color_t){160,240,255}) : (color_t){110,110,120};
    TFT_print("ENV", x, y - fh - 2);
    float ta = sy.atk, td = sy.dec, tr = sy.rel, tsum = ta + td + tr;
    if (tsum < 1e-4f) tsum = 1e-4f;
    float body = (float)(w - 4) * 0.72f;                  // A+D+R share 72%, sustain plateau the rest
    int aw = (int)(body * ta / tsum), dw = (int)(body * td / tsum), rw = (int)(body * tr / tsum);
    int sw = (w - 4) - aw - dw - rw;
    int xb = x + 2, yb = y + h - 2, yt = y + 2;
    int ys = yb - (int)(sy.sus * (float)(h - 4));
    int adsr_sel = (s_live_sel >= slive_param0() && s_live_sel < slive_n());   // an A/D/S/R point is the focus
    // dim the envelope polyline unless it's the selected element
    color_t col = adsr_sel ? (color_t){60, 200, 120} : (color_t){30, 96, 58};
    int x1 = xb + aw, x2 = x1 + dw, x3 = x2 + sw, x4 = x3 + rw;
    // 2 px: the same segment offset in y AND in x, so the near-vertical attack
    // thickens too (a y-offset alone leaves a steep segment one pixel wide).
    // Every offset point stays inside the cleared rect (bottom = y+h+2).
    for (int o = 0; o < 3; o++) {
        int dx = (o == 2) ? 1 : 0, dy = (o == 1) ? 1 : 0;
        TFT_drawLine(xb + dx, yb + dy, x1 + dx, yt + dy, col);
        TFT_drawLine(x1 + dx, yt + dy, x2 + dx, ys + dy, col);
        TFT_drawLine(x2 + dx, ys + dy, x3 + dx, ys + dy, col);
        TFT_drawLine(x3 + dx, ys + dy, x4 + dx, yb + dy, col);
    }
    if (adsr_sel) {   // encoder-nav focus point (A/D/S/R)
        int mx = x1, my = yt;                              // param0 = A
        int k = s_live_sel - slive_param0();
        if (k == 1)      { mx = x2; my = ys; }
        else if (k == 2) { mx = x3; my = ys; }
        else if (k == 3) { mx = x4; my = yb; }
        // bigger, green selection dot (edit mode stays amber to read distinct)
        int r = s_live_edit ? 6 : 5;
        // keep the whole dot inside the cleared rect (right = x+w-1, bottom =
        // y+h+1) so the R point at the bottom-right corner leaves no artifact
        if (mx + r > x + w - 1) mx = x + w - 1 - r;
        if (my + r > y + h + 1) my = y + h + 1 - r;
        // ...and the LEFT/TOP as well: a short attack puts the A point at
        // xb = x+2, so a radius-5 dot straddled x=8 and left crumbs OUTSIDE
        // the cleared rect — invisible until something stopped repainting over
        // them (the LFO view switch is what exposed it)
        if (mx - r < x) mx = x + r;
        if (my - r < y - fh - 2) my = y - fh - 2 + r;
        color_t hc = s_live_edit ? (color_t){130, 255, 150} : (color_t){70, 255, 130};
        TFT_fillCircle(mx, my, r, hc);
    }
}

// ---- LFO section: the other view of the bottom strip ------------------------
// Same rect as the ADSR graph. Five cells, label over value, the focused one
// picked out. "div" is a clock division when sync is on and a free rate in Hz
// when it is off — the cell relabels itself rather than showing a dead control.
static void draw_lfo(void)
{
    int fh = TFT_getfontheight();
    int x = 8, y = fh + 16 + 104 + 32, w = _width - 16, h = 40;
    _bg = TFT_BLACK; CLEAR_RECT(x, y - fh - 2, w, h + fh + 4);

    int tf = (s_live_sel == slive_title_el());
    _fg = tf ? (s_live_edit ? (color_t){130,255,150} : (color_t){160,240,255}) : (color_t){110,110,120};
    TFT_print("LFO", x, y - fh - 2);
    // the live tempo sits next to the title so sync is legible at a glance
    float bpm = clock_core_beat_bpm();
    _fg = (color_t){70,70,80};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    char t[28];
    if (sy.lfo_sync) snprintf(t, sizeof(t), bpm > 0 ? "clk %.1f" : "no clock", bpm);
    else             snprintf(t, sizeof(t), "free");
    TFT_print(t, x + 34, y - fh);
    TFT_setFont(DEFAULT_FONT, NULL);

    static const char *lab[SLIVE_LFO_N] = { "sync", "div", "shape", "amt", "dest" };
    char val[SLIVE_LFO_N][12];
    snprintf(val[0], 12, "%s", sy.lfo_sync ? "on" : "off");
    if (sy.lfo_sync) snprintf(val[1], 12, "%s", lfo_div_name(sy.lfo_div));
    else             snprintf(val[1], 12, "%.1fHz", sy.lfo_rate);
    snprintf(val[2], 12, "%s", lfo_shape_name(sy.lfo_shape));
    snprintf(val[3], 12, "%.0f%%", sy.lfo_depth * 100.0f);
    snprintf(val[4], 12, "%s", sy.lfo_dest == LFO_CUT ? "cutoff" : sy.lfo_dest == LFO_PITCH ? "pitch" : "off");

    int cw = w / SLIVE_LFO_N;
    for (int i = 0; i < SLIVE_LFO_N; i++) {
        int cx = x + i * cw + cw / 2, f = (s_live_sel == slive_param0() + i);
        _fg = f ? (color_t){130,130,140} : (color_t){80,80,90};
        TFT_setFont(DEF_SMALL_FONT, NULL);
        int lw = TFT_getStringWidth((char*)lab[i]);
        TFT_print((char*)lab[i], cx - lw / 2, y + 2);
        TFT_setFont(DEFAULT_FONT, NULL);
        // a dest of "off" means the LFO is audible nowhere — say so by dimming
        bool dim = (sy.lfo_dest == LFO_OFF) && i != 4;
        _fg = f ? (s_live_edit ? (color_t){130,255,150} : (color_t){160,240,255})
                : dim ? (color_t){70,70,80} : (color_t){190,190,200};
        int vw = TFT_getStringWidth(val[i]);
        TFT_print(val[i], cx - vw / 2, y + 16);
    }
}
static unsigned lfo_sig(void)
{
    return (unsigned)sy.lfo_sync * 3u + (unsigned)sy.lfo_div * 7u + (unsigned)sy.lfo_shape * 13u
         + (unsigned)(sy.lfo_depth * 1000.0f) * 17u + (unsigned)sy.lfo_dest * 19u
         + (unsigned)(sy.lfo_rate * 100.0f) * 23u;
}
static unsigned adsr_sig(void)
{
    return (unsigned)(sy.atk*1000)*7u + (unsigned)(sy.dec*1000)*13u
         + (unsigned)(sy.sus*1000)*17u + (unsigned)(sy.rel*1000)*19u;
}
// one entry point for the bottom strip, whichever view it is showing
static void draw_bottom(void) { if (s_env_view) draw_lfo(); else draw_adsr(); }
static unsigned bottom_sig(void) { return s_env_view ? lfo_sig() : adsr_sig(); }

// the osc preview only depends on engine + timbre (+ FM ratio) — NOT on the
// cut/res/env dials, so it doesn't repaint on their knob/CV jitter.
static unsigned osc_sig(void)
{
    float t = sy.engine == ENG_FM ? sy.fm_index : sy.engine == ENG_WT ? sy.fold : sy.shape;
    unsigned r = (unsigned)sy.engine * 101u + (unsigned)(t * 1000.0f) * 37u
               + (s_live_sel == 0 ? (s_live_edit ? 2u : 1u) : 0u);   // focus box too
    if (sy.engine == ENG_FM) r += (unsigned)(sy.fm_ratio * 100.0f) * 53u;
    return r;
}

static void live_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    s_skip_clear = true;                 // the screen is already black
    draw_header();
    draw_osc();         s_sig_osc   = osc_sig();
    draw_dials();
    draw_bottom();      s_sig_adsr  = bottom_sig();
    s_skip_clear = false;
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn:pick  press:edit  hold:back    CV1:pitch TR1:gate", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

// edit the focused element by a detent (dials re-arm knob takeover)
static void slive_edit(int dir)
{
    float d = (float)dir;
    switch (s_live_sel) {
        case 0:   // synth type / engine (VA / FM / WT); osc preview + K5 follow
            sy.engine += dir;
            if (sy.engine < 0) sy.engine = ENG_WT;
            if (sy.engine > ENG_WT) sy.engine = ENG_VA;
            cvmtx_rearm(&sy.mtx);   // K5 timbre is engine-specific -> recapture
            break;
        case 1:   // timbre — engine-aware
            if (sy.engine == ENG_FM)      sy.fm_index = sclampf(sy.fm_index + d * 0.25f, 0.0f, 8.0f);
            else if (sy.engine == ENG_WT) sy.fold     = sclampf(sy.fold + d * 0.05f, 0.0f, 1.0f);
            else                          sy.shape    = sclampf(sy.shape + d * 0.05f, 0.0f, 1.0f);
            break;
        case 2: sy.cutoff_base = sclampf(sy.cutoff_base * (dir > 0 ? 1.06f : 0.94f), 30.0f, 12000.0f); break;
        case 3: sy.res01       = sclampf(sy.res01 + d * 0.05f, 0.0f, 1.0f); break;
        case 4: sy.env_to_cut  = sclampf(sy.env_to_cut + d * 0.05f, 0.0f, 1.0f); break;
        default: break;
    }
    if (s_live_sel >= slive_param0()) {
        int k = s_live_sel - slive_param0();
        if (!s_env_view) {                       // ENV view: A D S R
            switch (k) {
                case 0: sy.atk = sclampf(sy.atk + d * 0.005f, 0.0005f, 2.0f); break;
                case 1: sy.dec = sclampf(sy.dec + d * 0.01f, 0.001f, 2.0f); break;
                case 2: sy.sus = sclampf(sy.sus + d * 0.05f, 0.0f, 1.0f); break;
                case 3: sy.rel = sclampf(sy.rel + d * 0.02f, 0.001f, 3.0f); break;
            }
        } else {                                 // LFO view
            switch (k) {
                case 0: sy.lfo_sync = !sy.lfo_sync; break;
                case 1:
                    if (sy.lfo_sync) { sy.lfo_div += dir;
                                       if (sy.lfo_div < 0) sy.lfo_div = LFO_DIV_N - 1;
                                       if (sy.lfo_div >= LFO_DIV_N) sy.lfo_div = 0; }
                    else             sy.lfo_rate = sclampf(sy.lfo_rate + d * 0.25f, 0.05f, 20.0f);
                    break;
                case 2: sy.lfo_shape += dir;
                        if (sy.lfo_shape < 0) sy.lfo_shape = LFO_SHAPE_N - 1;
                        if (sy.lfo_shape >= LFO_SHAPE_N) sy.lfo_shape = 0;
                        break;
                case 3: sy.lfo_depth = sclampf(sy.lfo_depth + d * 0.05f, 0.0f, 1.0f); break;
                case 4: sy.lfo_dest += dir;
                        if (sy.lfo_dest < 0) sy.lfo_dest = LFO_PITCH;
                        if (sy.lfo_dest > LFO_PITCH) sy.lfo_dest = LFO_OFF;
                        break;
            }
        }
    }
    if (s_live_sel >= 1 && s_live_sel <= 4) cvmtx_rearm(&sy.mtx);   // re-arm takeover
}

// repaint the element that lost focus and the one that gained it (or just
// the focused one on an edit). Elements: 0 = header tag + osc trace,
// 1..4 = dials, 5..8 = ADSR points (one strip).
static void slive_repaint_el(int e)
{
    if (e == 0)       { draw_header(); draw_osc(); s_sig_osc = osc_sig(); }
    else if (e < SLIVE_BASE) { draw_dial(e - 1); }
    else              { draw_bottom(); s_sig_adsr = bottom_sig(); }
}
static void slive_repaint(int prev_sel)
{
    slive_repaint_el(prev_sel);
    if (s_live_sel != prev_sel) slive_repaint_el(s_live_sel);
    // an edit can move values other elements show (engine change -> dials)
    draw_dials_changed();
    unsigned os = osc_sig();  if (os != s_sig_osc)  { draw_osc();  s_sig_osc  = os; }
    unsigned as = bottom_sig(); if (as != s_sig_adsr) { draw_bottom(); s_sig_adsr = as; }
}

static int synth_live_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_FAST: {
            // cheap: note readout only. The heavy element redraws (osc/dials/
            // adsr) are gated to the SLOW tick below — knob/CV ADC jitter was
            // redrawing them every 300ms, and those TFT + PSRAM shadow-framebuffer
            // writes contend with the PSRAM audio path (noise / audio loss).
            if (cur_midi() != s_last_note) draw_note();   // the note field only
            break;
        }
        case EV_TIMER_REPEATING_SLOW: {
            // each block redraws only when its own values change (no free-running meter)
            if (cur_midi() != s_last_note) draw_note();   // gate no longer recolors
            unsigned os = osc_sig();
            if (os != s_sig_osc) { draw_osc(); s_sig_osc = os; }
            draw_dials_changed();                          // one dial per knob move, not four
            unsigned as = bottom_sig();
            if (as != s_sig_adsr) { draw_bottom(); s_sig_adsr = as; }
            break;
        }
        case EV_FWD: {
            int prev = s_live_sel;
            if (s_live_edit) slive_edit(+1);
            else s_live_sel = (s_live_sel + 1) % slive_n();
            slive_repaint(prev);
            break;
        }
        case EV_BWD: {
            int prev = s_live_sel;
            if (s_live_edit) slive_edit(-1);
            else s_live_sel = (s_live_sel + slive_n() - 1) % slive_n();
            slive_repaint(prev);
            break;
        }
        case EV_SHORT_PRESS:
            if (s_live_sel == slive_title_el()) {
                // the title is a SWITCH, not a value: flip ENV <-> LFO and keep
                // the focus on it (the two views have different element counts,
                // so landing anywhere else would need clamping anyway)
                s_env_view = !s_env_view;
                s_live_edit = false;
                s_live_sel = slive_title_el();
                draw_bottom(); s_sig_adsr = bottom_sig();
            } else {
                s_live_edit = !s_live_edit;   // click in/out (element 0 edits engine type)
                slive_repaint(s_live_sel);
            }
            break;
        case EV_LONG_PRESS:
            if (s_live_edit) { s_live_edit = false; slive_repaint(s_live_sel); }   // escape edit
            else return M_SYNTH_SETUP;                                   // leave Live
            break;
        default: break;
    }
    return 0;
}

// ---- Setup (shared framework: press cycles TOGGLEs, [ ] edits RANGEs) -------
// Instrument params live here; all five effects moved to a dedicated FX
// sub-page (the "FX" action row) so this list stays readable.
static const setup_item_t sy_setup_items[] = {
    {"Engine",    ST_TOGGLE}, {"Base Note", ST_RANGE},  {"Quantize",  ST_TOGGLE},
    {"Shape",     ST_RANGE},  {"FM Ratio",  ST_RANGE},  {"FM Index",  ST_RANGE},
    {"Attack",    ST_RANGE},  {"Decay",     ST_RANGE},  {"Sustain",   ST_RANGE},
    {"Release",   ST_RANGE},  {"Env>Cut",   ST_RANGE},  {"Glide",     ST_RANGE},
    {"LFO Rate",  ST_RANGE},  {"LFO Depth", ST_RANGE},  {"LFO Dest",  ST_TOGGLE},
    {"LFO Sync",  ST_TOGGLE}, {"LFO Div",   ST_TOGGLE}, {"LFO Shape", ST_TOGGLE},
    {"Level",     ST_RANGE},
    {"Load Wave", ST_ACTION}, {"CV Matrix", ST_ACTION},
    {"FX1",        ST_ACTION}, {"FX2",       ST_ACTION}, {"FX3 Reverb", ST_ACTION},
    {"Save Patch", ST_ACTION}, {"Load Patch", ST_ACTION},
};

// last saved patch id, shown inline on the Save Patch row as confirmation
static char s_last_saved[12];

// FX rack slot editor state (shared fxrack machinery drives the rows)
static int s_cur_slot = 0;       // slot the FX sub-page is editing
static int s_setup_return = -1;  // Setup row to restore on return from a sub-page
static setup_menu_t sy_fx;       // defined below
static setup_item_t s_syfx_items[FXRACK_MAXROWS];
static int8_t s_syfx_param[FXRACK_MAXROWS];
static int s_syfx_n;

static void syfx_rebuild(void)
{
    s_syfx_n = fxrack_menu_rows(&sy_rk, s_cur_slot, s_syfx_items, s_syfx_param);
    sy_fx.n = s_syfx_n;
    sy_fx.title = s_cur_slot == 0 ? "Synth FX1" : s_cur_slot == 1 ? "Synth FX2" : "Synth FX3";
    if (sy_fx.sel >= s_syfx_n) sy_fx.sel = s_syfx_n - 1;
    if (sy_fx.sel < 0) sy_fx.sel = 0;
}

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
        case 15: snprintf(v, n, "%s", sy.lfo_sync ? "on" : "off"); break;
        case 16: snprintf(v, n, "%s", lfo_div_name(sy.lfo_div)); break;
        case 17: snprintf(v, n, "%s", lfo_shape_name(sy.lfo_shape)); break;
        case 18: snprintf(v, n, "%.0f%%", sy.level * 100.0f); break;
        case 19: snprintf(v, n, "%s", sy.wave_name[0] ? sy.wave_name : "(none)"); break;
        case 20: { int on = 0; for (int d = 0; d < SYM_N; d++) if (sy.mtx.src[d] >= 0) on++;
                   if (on) snprintf(v, n, "%d on >", on); else snprintf(v, n, "edit >"); break; }
        case 21: snprintf(v, n, "%s >", fxrack_slot_name(&sy_rk, 0)); break;
        case 22: snprintf(v, n, "%s >", fxrack_slot_name(&sy_rk, 1)); break;
        case 23: snprintf(v, n, "%s >", fxrack_slot_name(&sy_rk, 2)); break;
        case 24: snprintf(v, n, "%s", s_last_saved[0] ? s_last_saved : "save >"); break;
        case 25: snprintf(v, n, "load >"); break;
    }
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
        case 15: sy.lfo_sync = !sy.lfo_sync; break;
        case 16: {
            sy.lfo_div += dir;
            if (sy.lfo_div < 0) sy.lfo_div = LFO_DIV_N - 1;
            else if (sy.lfo_div >= LFO_DIV_N) sy.lfo_div = 0;
            break;
        }
        case 17: {
            sy.lfo_shape += dir;
            if (sy.lfo_shape < 0) sy.lfo_shape = LFO_SHAPE_N - 1;
            else if (sy.lfo_shape >= LFO_SHAPE_N) sy.lfo_shape = 0;
            break;
        }
        case 18: sy.level += d * 0.05f; if (sy.level < 0) sy.level = 0; if (sy.level > 1) sy.level = 1; break;
    }
}

static int sy_setup_action(int i)
{
    if (i == 19) return M_SYNTH_LOAD;      // Load Wave -> browser
    if (i == 20) { s_setup_return = 20; return M_SYNTH_MATRIX; }   // CV Matrix
    if (i == 21) { s_setup_return = 21; s_cur_slot = 0; return M_SYNTH_FX; }   // FX1
    if (i == 22) { s_setup_return = 22; s_cur_slot = 1; return M_SYNTH_FX; }   // FX2
    if (i == 23) { s_setup_return = 23; s_cur_slot = 2; return M_SYNTH_FX; }   // FX3 reverb
    if (i == 24) {                         // Save Patch: mint + write, stay on Setup
        if (synth_patch_save(s_last_saved, sizeof(s_last_saved)) != 0)
            snprintf(s_last_saved, sizeof(s_last_saved), "err");
        return 0;                          // framework redraws -> row shows the id
    }
    if (i == 25) return M_SYNTH_PATCH;     // Load Patch -> patch browser
    return 0;
}

static setup_menu_t sy_setup = {
    .items = sy_setup_items,
    // COUNT THE TABLE, never a literal: this was 23 while the table grew to 26
    // when the LFO rows went in, which silently dropped FX3 Reverb, Save Patch
    // and Load Patch off the end of the page
    .n = (int)(sizeof(sy_setup_items) / sizeof(sy_setup_items[0])),
    .title = "Synth Setup",
    .aff_label = "Machine", .aff_target = M_MORE,
    .live_target = M_SYNTH_LIVE,
    .render = setup_val, .adjust = sy_adj, .action = sy_setup_action,
};

static int synth_setup_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU && s_setup_return >= 0) {   // return from a sub-page
        int p = s_setup_return; s_setup_return = -1;
        setup_menu_enter_at(&sy_setup, p);
        return 0;
    }
    return setup_menu_event(&sy_setup, event);
}

// ---- FX slot editor: one slot at a time, rows driven by the shared fxrack ----
static void sy_fx_val(int i, char *v, size_t n)
{
    if (i < 0 || i >= s_syfx_n) { v[0] = 0; return; }
    fxrack_menu_val(&sy_rk, s_cur_slot, s_syfx_param[i], v, n);
}

static void sy_fx_adj(int i, int dir)
{
    if (i < 0 || i >= s_syfx_n) return;
    int p = s_syfx_param[i];
    fxrack_menu_adj(&sy_rk, s_cur_slot, p, dir);
    if (p < 0) syfx_rebuild();   // effect changed -> param rows changed
}

static setup_menu_t sy_fx = {
    .items = s_syfx_items,
    .n = 0,                       // set by syfx_rebuild()
    .title = "Synth FX",          // overwritten per-slot by syfx_rebuild()
    .aff_label = "Setup", .aff_target = M_SYNTH_SETUP,
    .live_target = M_SYNTH_SETUP, // long-press = up one level to Setup
    .render = sy_fx_val, .adjust = sy_fx_adj, .action = NULL,
};

static int synth_fx_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) syfx_rebuild();   // dynamic row set for s_cur_slot
    return setup_menu_event(&sy_fx, event);
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

// ---- CV Matrix page: the shared cvmtx widget drives everything --------------
// (this file used to own the page; it moved to components/menu/cvmtx.c when
// Tape became the third matrix host)
static int synth_matrix_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    return cvmtx_menu_event(&sy.mtx, event, "Synth CV Matrix",
                            M_SYNTH_SETUP, M_SYNTH_LIVE);
}

// ---- Load Patch: a scrollable list of usr/synth/PAT_NNN.jsn (newest first) --
#define SY_PATCH_MAX 64
static char s_patches[SY_PATCH_MAX][12];
static int  s_npatch = 0, s_patch_sel = 0;

static void patch_redraw(void)
{
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE; TFT_print("Load Patch", 6, 4);
    menuTFTPrintAffordance("Setup", s_patch_sel == -1);
    if (s_npatch == 0) {
        _fg = (color_t){150, 150, 160};
        TFT_print("(no saved patches)", 8, fh + 20);
        return;
    }
    int row_h = fh + 5, y0 = fh + 14;
    int vis = (_height - fh - 6 - y0) / row_h;
    if (vis < 1) vis = 1;
    int top = 0;
    if (s_patch_sel >= vis) top = s_patch_sel - vis + 1;
    for (int r = 0; r < vis; r++) {
        int i = top + r;
        if (i >= s_npatch) break;
        int y = y0 + r * row_h;
        _bg = (i == s_patch_sel) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == s_patch_sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 4, _bg);
        TFT_print(s_patches[i], 8, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn: pick   press: load   hold: back", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int synth_patch_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU:
            s_npatch = synth_patch_list(s_patches, SY_PATCH_MAX);
            s_patch_sel = s_npatch ? 0 : -1;
            patch_redraw();
            break;
        case EV_FWD:
            if (s_npatch) { s_patch_sel++; if (s_patch_sel >= s_npatch) s_patch_sel = -1; patch_redraw(); }
            break;
        case EV_BWD:
            if (s_npatch) { s_patch_sel--; if (s_patch_sel < -1) s_patch_sel = s_npatch - 1; patch_redraw(); }
            break;
        case EV_SHORT_PRESS:
            if (s_patch_sel < 0) return M_SYNTH_SETUP;          // affordance / empty
            synth_patch_load(s_patches[s_patch_sel]);
            return M_SYNTH_SETUP;
        case EV_LONG_PRESS: return M_SYNTH_SETUP;
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
    menusys_new_item(_ms, M_SYNTH_PATCH);  menusys_item_set_default_cb(_ms, M_SYNTH_PATCH, synth_patch_handler);
    menusys_new_item(_ms, M_SYNTH_FX);     menusys_item_set_default_cb(_ms, M_SYNTH_FX, synth_fx_handler);
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
    .setup = &sy_setup,
    .caps = MC_MIDI | MC_CLOCK | MC_MATRIX | MC_FX,
};
