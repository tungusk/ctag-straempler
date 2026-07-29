// Keys UI — a Live dashboard (waveform + loop box + playhead, four macro dials,
// ADSR curve) and a Setup page (shared setup_menu framework). Mirrors the Synth
// dashboard: black canvas, boxed elements, redraw-on-change only. The waveform
// strip shows the sustain-loop window as a box (the box IS the loop) with a
// white playhead — the deck/slicer/sampler3 idiom.
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
#include "sample_ram.h"
#include "pitch_detect.h"          // Auto-Tune row: name/cents for the verdict
#include "audio.h"                 // audio_proc_us() for the FX cost-guard
#include "instsampler_priv.h"

static const color_t GATE_ON = {40, 200, 90};    // note stays green (gate flips too fast to read)
static const color_t WF_DIM  = {85, 88, 100};    // waveform when NOT the focus (dimmer)
static const color_t WF_ORIG = {235, 238, 245};  // white zero/origin reference line
static const color_t LOOP_COL = {70, 200, 235};  // loop window (cyan)
static const color_t PH_COL  = {240, 240, 245};  // playhead (white)

// THE ZONE THE UI ACTS ON. Every per-zone row (sample, root, fine, loop) and the
// whole Live waveform reads through this, so the Zone row retargets all of them
// at once and nothing can be left pointing at zone 0 by accident. Clamped rather
// than trusted: edit_zone survives a Clear Zones that shrinks nzones under it.
static inline is_zone_t *ks_z(void)
{
    int i = inst.edit_zone;
    if (i < 0 || i >= inst.nzones) i = 0;
    return &inst.zone[i];
}

// WHAT THE LIVE PAGE SHOWS. Setup edits one zone (ks_z); Live is a PERFORMANCE
// view, so while a note is sounding it follows the zone that note actually chose
// — otherwise a multisample gives no sign it is one, the top row sits on whatever
// Setup last touched, and (worse) the playhead is drawn from the PLAYING voice's
// cursor while being SCALED by the EDIT zone's length, so it lands in the wrong
// place whenever the two differ. Falls back to the edit zone when nothing sounds.
static int kl_zone_idx(void)
{
    is_voice_t *v = &inst.voice[0];
    if (v->active && v->zone >= 0 && v->zone < inst.nzones && inst.zone[v->zone].frames)
        return v->zone;
    int i = inst.edit_zone;
    if (i < 0 || i >= inst.nzones) i = 0;
    return i;
}
static inline is_zone_t *kl_z(void) { return &inst.zone[kl_zone_idx()]; }
// inst.peaks holds ONE strip, so it has to be rebuilt when the shown zone moves
static int s_peaks_zone = -1;
static void kl_sync_peaks(void)
{
    int z = kl_zone_idx();
    if (z != s_peaks_zone) { keys_build_peaks_for(z); s_peaks_zone = z; }
}

static const char *const NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static void note_name_midi(int midi, char *buf, size_t n)
{
    if (midi < 0) { snprintf(buf, n, "--"); return; }
    if (midi > 127) midi = 127;
    snprintf(buf, n, "%s%d", NOTE_NAMES[midi % 12], midi / 12 - 1);
}

// ---- Live geometry ---------------------------------------------------------
static int L_wy(void) { return TFT_getfontheight() + 16; }
#define L_WX 8
#define L_WW (_width - 16)
#define L_WH 46

static int loop_x(uint32_t frame)
{
    uint32_t fr = kl_z()->frames;
    if (fr == 0) return L_WX;
    if (frame > fr) frame = fr;
    return L_WX + (int)((uint64_t)frame * L_WW / fr);
}

static int s_last_note = -9999;
// The header shows note, SOUNDING ZONE and gate state, so keying its repaint on
// the note alone missed a zone change at the same pitch and never un-greened the
// tag on release.
static unsigned s_sig_hdr = 0;
static unsigned s_sig_dials = 0, s_sig_adsr = 0, s_sig_wave = 0;
static int s_last_ph = -1;

// two-level encoder nav (slicer idiom): browse on-screen elements, click in to
// edit, long-press to escape. Named rather than numbered for the same reason the
// Setup rows are — these indices appear in four places and a literal in one of
// them silently edits the wrong thing.
enum {
    KL_SAMPLE = 0,                              // the waveform / file name
    KL_START, KL_CUT, KL_RES, KL_ENVCUT,        // the four dials
    KL_ATK, KL_DEC, KL_SUS, KL_REL,             // the ADSR points
    KL_LOOPS, KL_LOOPE,                         // the loop-window edges
    KL_N
};
#define KLIVE_N KL_N
static inline bool kl_is_loop(int e) { return e == KL_LOOPS || e == KL_LOOPE; }
static int  s_live_sel  = 1;      // focused element (start on the first dial)
static bool s_live_edit = false;  // false = browsing, true = editing the focus
// focus code for a drawn element: 0 none, 1 selected(browse), 2 editing
static int klive_focus(int elem) { return s_live_sel != elem ? 0 : (s_live_edit ? 2 : 1); }

// waveform trace colour: pops bright when the Sample/title element (sel 0) is
// the encoder focus, dimmer otherwise. Shared by the full strip + the
// per-column playhead-erase path so the two never disagree.
static inline color_t wf_color(void)
{
    return (s_live_sel == 0) ? (color_t){205, 210, 230} : WF_DIM;
}

// ---- header: title + sample tag + note name (green) ------------------------
static void draw_header(void)
{
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; TFT_fillRect(0, 0, _width, fh + 12, _bg);
    _fg = TFT_WHITE; TFT_print("Keys", 6, 4);
    int hf = klive_focus(0);   // Sample element focus highlights the file name
    _fg = hf == 2 ? (color_t){130, 255, 150} : hf == 1 ? (color_t){210, 190, 120} : (color_t){130, 130, 140};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    // In a multisample the strip below shows ONE zone, so say which — otherwise
    // the waveform and loop box silently belong to something you cannot identify.
    char tag[32];
    if (kl_is_loop(s_live_sel)) {
        // placing a loop edge by eye still wants the number, and the file name is
        // the least useful thing on screen at that moment
        is_zone_t *lz = kl_z();
        unsigned a = (unsigned)((uint64_t)lz->loop_start * 1000 / IS_RATE);
        unsigned b = (unsigned)((uint64_t)lz->loop_end * 1000 / IS_RATE);
        if (lz->loop_mode != LOOP_FWD)
            snprintf(tag, sizeof(tag), "loop OFF %u-%u", a, b);
        else if (s_live_sel == KL_LOOPS)
            snprintf(tag, sizeof(tag), "loop [%u-%u ms", a, b);
        else
            snprintf(tag, sizeof(tag), "loop %u-%u] ms", a, b);
        _fg = (color_t){160, 240, 255};
    }
    else if (inst.nzones > 1) {
        // WHICH ZONE IS SOUNDING. Arlo, playing the first multisample: "how do i
        // know its playing multisamples? the top row doesnt change" — because it
        // showed the zone SETUP was editing, which does not move while you play.
        // It now tracks the note, and goes green while one is held so the change
        // reads as live rather than as a label that happens to differ.
        snprintf(tag, sizeof(tag), "%d/%d %s", kl_zone_idx() + 1, inst.nzones,
                 kl_z()->sample[0] ? kl_z()->sample : "(none)");
        if (inst.voice[0].active) _fg = GATE_ON;
    }
    else
        snprintf(tag, sizeof(tag), "%s", kl_z()->sample[0] ? kl_z()->sample : "(no sample)");
    TFT_print(tag, 54, 6);
    TFT_setFont(DEFAULT_FONT, NULL);
    char nm[12]; note_name_midi((int)lroundf(inst.note_disp), nm, sizeof(nm));
    Font f = cfont; TFT_setFont(DEJAVU18_FONT, NULL);
    _fg = GATE_ON; TFT_print(nm, _width - 10 - TFT_getStringWidth(nm), 2);
    cfont = f;
}

// one waveform column at pixel x: black bg + grey peak + a loop edge line if it
// falls here (used to erase the playhead cleanly on the incremental path)
static void wave_col(int x)
{
    int wy = L_wy();
    int col = x - L_WX;
    if (col < 0 || col >= L_WW) return;
    _bg = TFT_BLACK; TFT_fillRect(x, wy, 1, L_WH, _bg);
    int pi = clampi(col * IS_PEAKS / L_WW, 0, IS_PEAKS - 1);
    int h = inst.peaks[pi] * (L_WH / 2) / (inst.peak_max ? inst.peak_max : 255);
    if (h > L_WH / 2) h = L_WH / 2;
    if (h < 1 && kl_z()->frames) h = 1;
    int cy = wy + L_WH / 2;
    if (h > 0) TFT_drawLine(x, cy - h, x, cy + h, wf_color());
    TFT_drawLine(x, cy, x, cy, WF_ORIG);   // keep the origin line continuous under the playhead sweep
    // MUST match draw_wave's edge rule, including the focus colours and the
    // second handle column — this path repaints one column behind the moving
    // playhead, so any disagreement shows up as the sweep quietly erasing a bit
    // of the edge you are currently dragging.
    bool loop_on = (kl_z()->loop_mode == LOOP_FWD);
    if ((loop_on || kl_is_loop(s_live_sel)) && kl_z()->frames) {
        int lsx = loop_x(kl_z()->loop_start), lex = loop_x(kl_z()->loop_end);
        for (int e = 0; e < 2; e++) {
            int ex = e ? lex : lsx;
            int elem = e ? KL_LOOPE : KL_LOOPS;
            int f = klive_focus(elem);
            int ex2 = (elem == KL_LOOPS) ? ex + 1 : ex - 1;
            if (x != ex && !(f && x == ex2)) continue;
            color_t c = !loop_on ? (color_t){45, 80, 95}
                      : f == 2  ? (color_t){130, 255, 150}
                      : f == 1  ? (color_t){160, 240, 255}
                                : LOOP_COL;
            TFT_drawLine(x, wy, x, wy + L_WH, c);
        }
    }
}

// full waveform strip: peaks + loop-window edges + center line
static void draw_wave(void)
{
    int wy = L_wy(), cy = wy + L_WH / 2;
    _bg = TFT_BLACK; TFT_fillRect(L_WX, wy, L_WW, L_WH, _bg);
    if (kl_z()->frames == 0) {
        // AN EMPTY SLOT STILL NEEDS A CURSOR. The Sample element's only focus cue
        // is the waveform colour, so with no waveform there was nothing at all to
        // see (Arlo 2026-07-26: "when the sample is empty there's no selection box
        // on the empty slot") — the one element you must navigate to in order to
        // FIX an empty Keys was the one element that could not show it was
        // selected. Borrow the dials' focus-ring idiom as a rectangle.
        int hf = klive_focus(0);
        if (hf) {
            color_t hc = hf == 2 ? (color_t){130, 255, 150} : (color_t){150, 150, 170};
            TFT_drawRect(L_WX, wy, L_WW, L_WH, hc);
            if (hf == 2) TFT_drawRect(L_WX + 1, wy + 1, L_WW - 2, L_WH - 2, hc);
        }
        // and SAY WHY if a load actually failed — a silent no-op is
        // indistinguishable from a browser that does not work
        char msg[40];   // TFT_print takes char*, not const char*
        snprintf(msg, sizeof(msg), "%s",
                 inst.load_err[0] ? inst.load_err : "no sample - Setup > Load Sample");
        _fg = inst.load_err[0] ? (color_t){240, 140, 120} : (color_t){90, 90, 100};
        TFT_print(msg, L_WX + 6, cy - TFT_getfontheight() / 2);
        if (inst.load_err[0]) {
            _fg = (color_t){150, 110, 100};
            TFT_print("reboot frees PSRAM", L_WX + 6, cy + TFT_getfontheight() / 2 + 2);
        }
        s_last_ph = -1;
        return;
    }
    // waveform POPS brighter when the Sample/title element is selected, dimmer otherwise
    color_t wcol = wf_color();
    for (int c = 0; c < L_WW; c++) {
        int pi = c * IS_PEAKS / L_WW;
        int h = inst.peaks[pi] * (L_WH / 2) / (inst.peak_max ? inst.peak_max : 255);
        if (h > L_WH / 2) h = L_WH / 2;
        if (h < 1) h = 1;
        TFT_drawLine(L_WX + c, cy - h, L_WX + c, cy + h, wcol);
    }
    // white zero/origin reference line across the strip
    TFT_drawLine(L_WX, cy, L_WX + L_WW - 1, cy, WF_ORIG);
    // Loop edges. Drawn when looping is ON, and ALSO while a loop edge is the
    // encoder focus even if it is off — otherwise browsing to it shows nothing at
    // all and you would be placing an invisible marker. Off + focused draws dim;
    // clicking in to edit is what turns looping on (see the handler).
    bool loop_on = (kl_z()->loop_mode == LOOP_FWD);
    if (loop_on || kl_is_loop(s_live_sel)) {
        int lsx = loop_x(kl_z()->loop_start), lex = loop_x(kl_z()->loop_end);
        for (int e = 0; e < 2; e++) {
            int ex = e ? lex : lsx;
            int elem = e ? KL_LOOPE : KL_LOOPS;
            int f = klive_focus(elem);
            color_t c = !loop_on ? (color_t){45, 80, 95}          // off: dim
                      : f == 2  ? (color_t){130, 255, 150}        // editing: green
                      : f == 1  ? (color_t){160, 240, 255}        // selected: bright
                                : LOOP_COL;
            TFT_drawLine(ex, wy, ex, wy + L_WH, c);
            // a focused edge gets a second column so it reads as a grabbed handle
            // rather than as one more 1 px line among the waveform
            if (f) {
                int ex2 = (elem == KL_LOOPS) ? ex + 1 : ex - 1;
                if (ex2 >= L_WX && ex2 < L_WX + L_WW)
                    TFT_drawLine(ex2, wy, ex2, wy + L_WH, c);
            }
        }
    }
    s_last_ph = -1;
}

static void draw_playhead(void)
{
    int wy = L_wy();
    is_voice_t *v = &inst.voice[0];
    int ph = -1;
    if (v->active && kl_z()->frames)
        ph = loop_x((uint32_t)v->pos);
    if (ph == s_last_ph) return;
    if (s_last_ph >= 0) wave_col(s_last_ph);
    if (ph >= 0) TFT_drawLine(ph, wy, ph, wy + L_WH, PH_COL);
    s_last_ph = ph;
}

// ---- a knob dial: ring + pointer, label + value (lifted from Synth) --------
static void dial(int cx, int cy, int r, float v01, const char *lab, const char *val, bool live, int hi)
{
    int fh = TFT_getfontheight();
    v01 = clampf(v01, 0.0f, 1.0f);
    _bg = TFT_BLACK;
    // clear generously: the pointer now ESCAPES past the ring (to r+7) and the
    // focus ring sits at r+4 — cover both + the label row so nothing is stranded
    int m = r + 8;
    TFT_fillRect(cx - m, cy - m, m * 2, m + r + fh + 8, _bg);
    if (hi) {                                          // encoder-nav focus ring
        color_t hc = hi == 2 ? (color_t){130, 255, 150} : (color_t){150, 150, 170};
        TFT_drawCircle(cx, cy, r + 3, hc);
        if (hi == 2) TFT_drawCircle(cx, cy, r + 4, hc);   // thicker while editing
    }
    color_t ring = live ? (color_t){0, 150, 220} : (color_t){70, 70, 80};
    TFT_drawCircle(cx, cy, r, ring);
    // pointer that ESCAPES the ring: a radial tick from just inside the rim to
    // past it, leaving the centre free for the value readout
    float ang = (-135.0f + v01 * 270.0f) * 0.01745329f;
    float sn = sinf(ang), cs = cosf(ang);
    color_t ncol = live ? (color_t){255, 255, 255} : (color_t){175, 175, 185};
    TFT_drawLine(cx + (int)(sn * (r - 3)), cy - (int)(cs * (r - 3)),
                 cx + (int)(sn * (r + 7)), cy - (int)(cs * (r + 7)), ncol);
    // value in the MIDDLE of the dial
    _fg = TFT_WHITE;
    TFT_print((char *)val, cx - TFT_getStringWidth((char *)val) / 2, cy - fh / 2);
    // label stays BELOW the circle
    _fg = (color_t){130, 130, 140};
    TFT_print((char *)lab, cx - TFT_getStringWidth((char *)lab) / 2, cy + r + 4);
}

static int dial_cy(void) { return L_wy() + L_WH + 8 + 28; }   // +8 lower: room for the bigger dials' escaping needle above

static void draw_dials(void)
{
    int cy = dial_cy(), r = 24;
    int cx[4] = { 40, 120, 200, 280 };   // recentred + slightly tighter for the bigger dials
    char v[16];
    snprintf(v, sizeof(v), "%.0f%%", inst.start_frac * 100.0f);
    dial(cx[0], cy, r, inst.start_frac, "start", v, inst.knob_live[0], klive_focus(1));
    float cv = logf(inst.cutoff_base / 10.0f) / logf(600.0f);
    if (inst.cutoff_base >= 1000.0f) snprintf(v, sizeof(v), "%.1fk", inst.cutoff_base / 1000.0f);
    else                             snprintf(v, sizeof(v), "%.0f", inst.cutoff_base);
    dial(cx[1], cy, r, cv, "cut", v, inst.knob_live[1], klive_focus(2));
    snprintf(v, sizeof(v), "%.0f%%", inst.res01 * 100.0f);
    dial(cx[2], cy, r, inst.res01, "res", v, inst.knob_live[2], klive_focus(3));
    snprintf(v, sizeof(v), "%.0f%%", inst.env_to_cut * 100.0f);
    dial(cx[3], cy, r, inst.env_to_cut, "env>f", v, inst.knob_live[3], klive_focus(4));
}

// ---- ADSR curve (lifted from Synth) ----------------------------------------
static void draw_adsr(void)
{
    int fh = TFT_getfontheight();
    // +21 not +14: at +14 the ENV label/box top overdraws the dials' value row
    // (seen on the first shadow-FB screenshot — "53%" clipped, label cut to "El")
    int x = 8, y = dial_cy() + 20 + 2 * fh + 21, w = _width - 16, h = 38;
    _bg = TFT_BLACK; TFT_fillRect(x, y - fh - 2, w, h + fh + 4, _bg);
    _fg = (color_t){110, 110, 120}; TFT_print("ENV", x, y - fh - 2);
    float ta = inst.atk, td = inst.dec, tr = inst.rel, tsum = ta + td + tr;
    if (tsum < 1e-4f) tsum = 1e-4f;
    float body = (float)(w - 4) * 0.72f;
    int aw = (int)(body * ta / tsum), dw = (int)(body * td / tsum), rw = (int)(body * tr / tsum);
    int sw = (w - 4) - aw - dw - rw;
    int xb = x + 2, yb = y + h - 2, yt = y + 2;
    int ys = yb - (int)(inst.sus * (float)(h - 4));
    int adsr_sel = (s_live_sel >= 5 && s_live_sel <= 8);   // an A/D/S/R point is the focus
    // dim the envelope polyline unless it's the selected element
    color_t col = adsr_sel ? (color_t){60, 200, 120} : (color_t){30, 96, 58};
    int x1 = xb + aw, x2 = x1 + dw, x3 = x2 + sw, x4 = x3 + rw;
    TFT_drawLine(xb, yb, x1, yt, col);
    TFT_drawLine(x1, yt, x2, ys, col);
    TFT_drawLine(x2, ys, x3, ys, col);
    TFT_drawLine(x3, ys, x4, yb, col);
    // encoder-nav focus marker on the selected point (5=A 6=D 7=S 8=R)
    if (adsr_sel) {
        int mx = x1, my = yt;
        if (s_live_sel == 6) { mx = x2; my = ys; }
        else if (s_live_sel == 7) { mx = x3; my = ys; }
        else if (s_live_sel == 8) { mx = x4; my = yb; }
        // bigger, green selection dot (edit mode stays amber to read distinct)
        int r = s_live_edit ? 6 : 5;
        // keep the whole dot inside the cleared rect (right = x+w-1, bottom =
        // y+h+1) so the R point at the bottom-right corner leaves no artifact
        if (mx + r > x + w - 1) mx = x + w - 1 - r;
        if (my + r > y + h + 1) my = y + h + 1 - r;
        color_t hc = s_live_edit ? (color_t){130, 255, 150} : (color_t){70, 255, 130};
        TFT_fillCircle(mx, my, r, hc);
    }
}

static unsigned dials_sig(void)
{
    return (unsigned)(inst.cutoff_base) * 131u
         + (unsigned)(inst.start_frac * 1000.0f) * 17u
         + (unsigned)(inst.res01 * 1000.0f) * 29u
         + (unsigned)(inst.env_to_cut * 1000.0f) * 41u
         + (inst.knob_live[0]?1u:0u) + (inst.knob_live[1]?2u:0u)
         + (inst.knob_live[2]?4u:0u) + (inst.knob_live[3]?8u:0u);
}
static unsigned adsr_sig(void)
{
    return (unsigned)(inst.atk*1000)*7u + (unsigned)(inst.dec*1000)*13u
         + (unsigned)(inst.sus*1000)*17u + (unsigned)(inst.rel*1000)*19u;
}
static unsigned wave_sig(void)
{
    unsigned h = kl_z()->frames * 2654435761u;
    h ^= kl_z()->loop_start * 40503u;
    h ^= kl_z()->loop_end * 2246822519u;
    h ^= (unsigned)kl_z()->loop_mode * 7u;
    // edit_zone explicitly: two zones holding the SAME sample (a duplicate, or a
    // set built from one file) hash identically on every other field, so without
    // this the strip would not repaint when you moved between them
    h ^= (unsigned)kl_zone_idx() * 2654435789u;
    h ^= (unsigned)inst.nzones * 40961u;
    for (const char *p = kl_z()->sample; *p; p++) h = h * 31u + (unsigned)*p;
    return h;
}

static void live_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    draw_header();  s_last_note = (int)lroundf(inst.note_disp);
    draw_wave();    s_sig_wave = wave_sig();
    draw_playhead();
    draw_dials();   s_sig_dials = dials_sig();
    draw_adsr();    s_sig_adsr = adsr_sig();
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn:pick  press:edit  hold:back    CV1:pitch TR1:gate", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

// edit the focused element by a detent (dials re-arm knob takeover so the value
// sticks until a physical knob is moved)
static void klive_edit(int dir)
{
    float d = (float)dir;
    switch (s_live_sel) {
        case 1: inst.start_frac  = clampf(inst.start_frac + d * 0.02f, 0.0f, 0.99f); break;
        case 2: inst.cutoff_base = clampf(inst.cutoff_base * (dir > 0 ? 1.06f : 0.94f), 30.0f, 12000.0f); break;
        case 3: inst.res01       = clampf(inst.res01 + d * 0.05f, 0.0f, 1.0f); break;
        case 4: inst.env_to_cut  = clampf(inst.env_to_cut + d * 0.05f, 0.0f, 1.0f); break;
        case 5: inst.atk = clampf(inst.atk + d * 0.005f, 0.0005f, 2.0f); break;
        case 6: inst.dec = clampf(inst.dec + d * 0.01f, 0.001f, 2.0f); break;
        case 7: inst.sus = clampf(inst.sus + d * 0.05f, 0.0f, 1.0f); break;
        case 8: inst.rel = clampf(inst.rel + d * 0.02f, 0.001f, 3.0f); break;
        case KL_LOOPS: case KL_LOOPE: {
            is_zone_t *z = kl_z();
            if (!z->frames) break;
            // ONE DETENT ~= ONE PIXEL of the strip. This is the by-eye control —
            // the fixed 10 ms step Setup uses is 0.13 px on a 22 s sample, so the
            // box would not visibly move however long you turned. Floored at ~5 ms
            // so a very short sample does not become impossible to place finely.
            long step = (long)(z->frames / (uint32_t)L_WW);
            long floor_step = IS_RATE / 200;
            if (step < floor_step) step = floor_step;
            if (s_live_sel == KL_LOOPS) {
                long v = (long)z->loop_start + dir * step;
                if (v < 0) v = 0;
                if (v > (long)z->loop_end - 64) v = (long)z->loop_end - 64;
                if (v < 0) v = 0;
                v = (long)keys_snap_zero((uint32_t)v);       // declick the seam
                if (v >= (long)z->loop_end) v = (long)z->loop_end - 64;
                if (v < 0) v = 0;
                z->loop_start = (uint32_t)v;
            } else {
                long v = (long)z->loop_end + dir * step;
                if (v < (long)z->loop_start + 64) v = (long)z->loop_start + 64;
                if (v > (long)z->frames) v = (long)z->frames;
                v = (long)keys_snap_zero((uint32_t)v);
                if (v > (long)z->frames) v = (long)z->frames;
                if (v <= (long)z->loop_start) v = (long)z->loop_start + 64;
                z->loop_end = (uint32_t)v;
            }
        } break;
    }
    if (s_live_sel >= 1 && s_live_sel <= 4) inst.knob_ctx = -1;   // re-arm takeover
}

// redraw the interactive elements (focus rings + values), no full-screen clear
static void klive_repaint(void)
{
    draw_header();                       // Sample-element focus = file-name colour
    draw_wave();  s_sig_wave  = wave_sig();
    draw_playhead();
    draw_dials(); s_sig_dials = dials_sig();
    draw_adsr();  s_sig_adsr  = adsr_sig();
}

static int keys_live_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_FAST: {
            // cheap + frequent: playhead + note readout ONLY. The heavy element
            // redraws (dials/wave/adsr) are gated to the SLOW tick below — knob/
            // CV ADC jitter was redrawing the dials every 300ms, and those TFT +
            // PSRAM shadow-framebuffer writes contend with the PSRAM audio path
            // (Arlo: "noise and eventual loss of audio on the keys live screen").
            int midi = (int)lroundf(inst.note_disp);
            unsigned hs = (unsigned)(midi + 200) * 131u
                        + (unsigned)kl_zone_idx() * 17u
                        + (inst.voice[0].active ? 7u : 0u);
            if (hs != s_sig_hdr) { draw_header(); s_sig_hdr = hs; s_last_note = midi; }
            kl_sync_peaks();
            draw_playhead();
            break;
        }
        case EV_TIMER_REPEATING_SLOW: {
            int midi = (int)lroundf(inst.note_disp);
            unsigned hs2 = (unsigned)(midi + 200) * 131u
                         + (unsigned)kl_zone_idx() * 17u
                         + (inst.voice[0].active ? 7u : 0u);
            if (hs2 != s_sig_hdr) { draw_header(); s_sig_hdr = hs2; s_last_note = midi; }
            kl_sync_peaks();
            unsigned ws = wave_sig();
            if (ws != s_sig_wave) { draw_wave(); s_sig_wave = ws; }
            draw_playhead();
            unsigned ds = dials_sig();
            if (ds != s_sig_dials) { draw_dials(); s_sig_dials = ds; }
            unsigned as = adsr_sig();
            if (as != s_sig_adsr) { draw_adsr(); s_sig_adsr = as; }
            break;
        }
        case EV_FWD:
            if (s_live_edit) klive_edit(+1);
            else s_live_sel = (s_live_sel + 1) % KLIVE_N;
            klive_repaint();
            break;
        case EV_BWD:
            if (s_live_edit) klive_edit(-1);
            else s_live_sel = (s_live_sel + KLIVE_N - 1) % KLIVE_N;
            klive_repaint();
            break;
        case EV_SHORT_PRESS:
            if (s_live_sel == KL_SAMPLE) return M_ISMP_LOAD;   // Sample -> browser
            // Clicking INTO a loop edge turns looping on. Browsing to it only
            // shows you where the window sits (dim); committing to edit it is an
            // unambiguous statement that you want a loop, and without this the
            // control would silently do nothing until you went to Setup to arm it.
            // Reversible from Setup > Loop Mode, and the box appearing is the cue.
            if (!s_live_edit && kl_is_loop(s_live_sel) && kl_z()->frames)
                kl_z()->loop_mode = LOOP_FWD;
            s_live_edit = !s_live_edit;                // click in / out of edit
            klive_repaint();
            break;
        case EV_LONG_PRESS:
            if (s_live_edit) { s_live_edit = false; klive_repaint(); }   // escape edit
            else return M_ISMP_SETUP;                                    // leave Live
            break;
        default: break;
    }
    return 0;
}

// ---- Setup (shared framework) ----------------------------------------------
// Instrument params live here; all five effects moved to a dedicated FX
// sub-page (the "FX" action row) so this list stays readable.
// Rows are addressed by NAME, not by literal index. The value/adjust/action
// switches below used bare integers, and adding the Zone rows meant renumbering
// 24 cases across three of them by hand — where one wrong number silently makes
// a row edit a different parameter, which is invisible until someone turns it.
enum {
    KR_ZONE = 0, KR_LOAD, KR_ADDZONE, KR_CLEARZONES,
    KR_ROOT, KR_FINE, KR_AUTOTUNE, KR_TUNELOAD,
    KR_BASE, KR_QUANT,
    KR_LOOPMODE, KR_LOOPSTART, KR_LOOPEND, KR_LOOPXFADE,
    KR_ATK, KR_DEC, KR_SUS, KR_REL, KR_ENVCUT, KR_GLIDE, KR_LEVEL,
    KR_MATRIX, KR_FX1, KR_FX2, KR_FX3, KR_SAVE, KR_LOADPAT,
    KR_N
};

// Instrument params live here; all five effects moved to a dedicated FX
// sub-page (the "FX" action row) so this list stays readable. The four rows
// above Root act on ONE ZONE — the one the Zone row selects.
static const setup_item_t ks_setup_items[] = {
    {"Zone",         ST_RANGE},  {"Load Sample", ST_ACTION},
    {"Add Zone",     ST_ACTION}, {"Clear Zones", ST_ACTION},
    {"Root Note",    ST_RANGE},  {"Fine",        ST_RANGE},
    {"Auto-Tune",    ST_ACTION}, {"Tune on Load", ST_TOGGLE},
    {"Base Note",    ST_RANGE},
    {"Quantize",     ST_TOGGLE},
    {"Loop Mode",    ST_TOGGLE}, {"Loop Start",  ST_RANGE},
    {"Loop End",     ST_RANGE},  {"Loop Xfade",  ST_RANGE},
    {"Attack",       ST_RANGE},  {"Decay",       ST_RANGE},  {"Sustain",   ST_RANGE},
    {"Release",      ST_RANGE},  {"Env>Cut",     ST_RANGE},  {"Glide",     ST_RANGE},
    {"Level",        ST_RANGE},
    {"CV Matrix",    ST_ACTION},
    {"FX1",          ST_ACTION}, {"FX2",         ST_ACTION}, {"FX3 Reverb", ST_ACTION},
    {"Save Patch",   ST_ACTION}, {"Load Patch",  ST_ACTION},
};
_Static_assert(sizeof(ks_setup_items) / sizeof(ks_setup_items[0]) == KR_N,
               "ks_setup_items and the KR_* enum must stay in step");

// last saved patch id, shown inline on the Save Patch row as confirmation
static char s_last_saved[12];

static int s_cur_slot = 0;      // which FX slot the shared FX sub-page is editing
static int s_setup_return = -1; // Setup row to restore on return from a sub-page
// Load Sample REPLACES the whole set, Add Zone APPENDS — same browser, and this
// says which one asked for it. Cleared as soon as the browser returns so a later
// plain load can never inherit it.
static bool s_browse_append = false;

static void ks_val(int i, char *v, size_t n)
{
    is_zone_t *z = ks_z();
    char nm[12];
    switch (i) {
        case KR_ZONE:
            if (inst.nzones <= 0) { snprintf(v, n, "(empty)"); break; }
            snprintf(v, n, "%d/%d %s", inst.edit_zone + 1, inst.nzones,
                     z->sample[0] ? z->sample : "(none)");
            break;
        case KR_LOAD: snprintf(v, n, "%s", z->sample[0] ? z->sample : "(none)"); break;
        case KR_ADDZONE:
            snprintf(v, n, "%s", inst.nzones >= IS_MAX_ZONES ? "full" : "add >");
            break;
        case KR_CLEARZONES:
            // says what it will actually do, so it is never a surprise
            if (inst.nzones > 1) snprintf(v, n, "drop %d >", inst.nzones - 1);
            else                 snprintf(v, n, "-");
            break;
        case KR_ROOT: note_name_midi(z->root, nm, sizeof(nm)); snprintf(v, n, "%s (%d)", nm, z->root); break;
        case KR_FINE: snprintf(v, n, "%+d c", (int)lroundf(z->fine * 100.0f)); break;
        case KR_AUTOTUNE: {   // the zone's OWN verdict + WHERE it came from, so a
                    // name-derived tuning never passes for a heard one. Per zone:
                    // inst.tune_src only holds the last LOAD's verdict, which says
                    // nothing about zone 2 of 6.
                    char hn[12];
                    // tune_hz/tune_conf are per-LOAD, not per-zone, so they may
                    // describe a different zone than the one on screen
                    bool cur = (inst.edit_zone == inst.last_tuned_zone);
                    switch (z->tune_src) {
                        case TUNE_NAME:
                            pitch_note_name(z->tune_hint, nm, sizeof(nm));
                            snprintf(v, n, "%s (name)", nm);
                            break;
                        case TUNE_CONFLICT:
                            note_name_midi(z->root, nm, sizeof(nm));
                            if (z->tune_hint >= 0) {
                                pitch_note_name(z->tune_hint, hn, sizeof(hn));
                                snprintf(v, n, "%s %+dc !%s", nm, (int)lroundf(z->fine * 100.0f), hn);
                            } else {
                                snprintf(v, n, "%s %+dc !name", nm, (int)lroundf(z->fine * 100.0f));
                            }
                            break;
                        case TUNE_AUDIO:
                        case TUNE_BOTH:
                            note_name_midi(z->root, nm, sizeof(nm));
                            snprintf(v, n, "%s %+dc%s", nm, (int)lroundf(z->fine * 100.0f),
                                     z->tune_src == TUNE_BOTH ? " (nm oct)" : "");
                            break;
                        default:
                            if (cur && inst.tune_hz > 0.0f) {   // heard something, too weak to apply
                                pitch_result_t r = { .hz = inst.tune_hz };
                                pitch_from_hz(inst.tune_hz, &r);
                                pitch_note_name(r.midi, nm, sizeof(nm));
                                snprintf(v, n, "unsure (%s?)", nm);
                            } else snprintf(v, n, "run >");
                            break;
                    }
                } break;
        case KR_TUNELOAD: snprintf(v, n, "%s", inst.autotune_load ? "ON" : "OFF"); break;
        case KR_BASE: note_name_midi(inst.base_note, nm, sizeof(nm)); snprintf(v, n, "%s (%d)", nm, inst.base_note); break;
        case KR_QUANT: snprintf(v, n, "%s", inst.quantize ? "ON" : "OFF"); break;
        case KR_LOOPMODE: snprintf(v, n, "%s", z->loop_mode == LOOP_FWD ? "Fwd" : "Off"); break;
        case KR_LOOPSTART: snprintf(v, n, "%u ms", (unsigned)((uint64_t)z->loop_start * 1000 / IS_RATE)); break;
        case KR_LOOPEND: snprintf(v, n, "%u ms", (unsigned)((uint64_t)z->loop_end * 1000 / IS_RATE)); break;
        case KR_LOOPXFADE: snprintf(v, n, "%u ms", (unsigned)((uint64_t)z->loop_xfade * 1000 / IS_RATE)); break;
        case KR_ATK: snprintf(v, n, "%d ms", (int)(inst.atk * 1000.0f)); break;
        case KR_DEC: snprintf(v, n, "%d ms", (int)(inst.dec * 1000.0f)); break;
        case KR_SUS: snprintf(v, n, "%.0f%%", inst.sus * 100.0f); break;
        case KR_REL: snprintf(v, n, "%d ms", (int)(inst.rel * 1000.0f)); break;
        case KR_ENVCUT: snprintf(v, n, "%.0f%%", inst.env_to_cut * 100.0f); break;
        case KR_GLIDE: snprintf(v, n, "%d ms", (int)(inst.glide * 1000.0f)); break;
        case KR_LEVEL: snprintf(v, n, "%.0f%%", inst.level * 100.0f); break;
        case KR_MATRIX: { int on = 0; for (int d = 0; d < ISM_N; d++) if (inst.mtx.src[d] >= 0) on++;
                   if (on) snprintf(v, n, "%d on >", on); else snprintf(v, n, "edit >"); break; }
        case KR_FX1: snprintf(v, n, "%s >", fxrack_slot_name(&inst_rk, 0)); break;
        case KR_FX2: snprintf(v, n, "%s >", fxrack_slot_name(&inst_rk, 1)); break;
        case KR_FX3: snprintf(v, n, "%s >", fxrack_slot_name(&inst_rk, 2)); break;
        case KR_SAVE: snprintf(v, n, "%s", s_last_saved[0] ? s_last_saved : "save >"); break;
        case KR_LOADPAT: snprintf(v, n, "load >"); break;
    }
}

static void ks_adj(int i, int dir)
{
    is_zone_t *z = ks_z();
    float d = (float)dir;
    long step = IS_RATE / 100;                        // ~10 ms for loop points
    switch (i) {
        case KR_ZONE:
            // retargets every per-zone row AND the Live waveform, so the preview
            // has to be rebuilt or it keeps showing the previous zone's audio
            if (inst.nzones > 0) {
                inst.edit_zone = clampi(inst.edit_zone + dir, 0, inst.nzones - 1);
                keys_build_peaks();
            }
            break;
        case KR_ROOT: z->root = (uint8_t)clampi((int)z->root + dir, 12, 108); break;
        case KR_FINE: z->fine = clampf(z->fine + d * 0.02f, -1.0f, 1.0f); break;   // 2 cents/detent
        case KR_TUNELOAD: inst.autotune_load = !inst.autotune_load; break;
        case KR_BASE: inst.base_note = clampi(inst.base_note + dir, 12, 108); break;
        case KR_QUANT: inst.quantize = !inst.quantize; break;
        case KR_LOOPMODE: z->loop_mode = (z->loop_mode == LOOP_FWD) ? LOOP_OFF : LOOP_FWD; break;
        case KR_LOOPSTART: if (z->frames) {            // Loop Start (zero-cross snapped)
                    long ls = clampi((int)((long)z->loop_start + dir * step), 0, (int)z->loop_end - 64);
                    if (ls < 0) ls = 0;
                    ls = (long)keys_snap_zero((uint32_t)ls);
                    if (ls >= (long)z->loop_end) ls = (long)z->loop_end - 64;
                    if (ls < 0) ls = 0;
                    z->loop_start = (uint32_t)ls;
                } break;
        case KR_LOOPEND: if (z->frames) {              // Loop End (zero-cross snapped)
                    long le = clampi((int)((long)z->loop_end + dir * step), (int)z->loop_start + 64, (int)z->frames);
                    le = (long)keys_snap_zero((uint32_t)le);
                    if (le > (long)z->frames) le = (long)z->frames;
                    if (le <= (long)z->loop_start) le = (long)z->loop_start + 64;
                    z->loop_end = (uint32_t)le;
                } break;
        case KR_LOOPXFADE: z->loop_xfade = (uint32_t)clampi((int)z->loop_xfade + dir * (IS_RATE / 1000), 0, 4096); break;
        case KR_ATK: inst.atk = clampf(inst.atk + d * 0.005f, 0.0005f, 2.0f); break;
        case KR_DEC: inst.dec = clampf(inst.dec + d * 0.01f, 0.001f, 2.0f); break;
        case KR_SUS: inst.sus = clampf(inst.sus + d * 0.05f, 0.0f, 1.0f); break;
        case KR_REL: inst.rel = clampf(inst.rel + d * 0.02f, 0.001f, 3.0f); break;
        case KR_ENVCUT: inst.env_to_cut = clampf(inst.env_to_cut + d * 0.05f, 0.0f, 1.0f); break;
        case KR_GLIDE: inst.glide = clampf(inst.glide + d * 0.02f, 0.0f, 2.0f); break;
        case KR_LEVEL: inst.level = clampf(inst.level + d * 0.05f, 0.0f, 1.0f); break;
    }
}

static int ks_action(int i)
{
    if (i == KR_LOAD)     { s_browse_append = false; return M_ISMP_LOAD; }
    if (i == KR_ADDZONE) {
        if (inst.nzones >= IS_MAX_ZONES) return 0;    // row already reads "full"
        s_browse_append = true;
        return M_ISMP_LOAD;
    }
    if (i == KR_CLEARZONES) {
        // Drops zones 1..n-1 and KEEPS zone 0, rather than emptying the machine.
        // A single click that leaves Keys silent with no undo is not a thing to
        // put on a menu; a full reset is what Load Sample already does.
        keys_keep_first_zone();
        keys_build_peaks();
        return 0;
    }
    if (i == KR_AUTOTUNE) { keys_autotune(); return 0; }   // detect + apply; row shows the verdict
    if (i == KR_MATRIX) { s_setup_return = KR_MATRIX; return M_ISMP_MATRIX; }
    if (i == KR_FX1) { s_setup_return = KR_FX1; s_cur_slot = 0; return M_ISMP_FX; }
    if (i == KR_FX2) { s_setup_return = KR_FX2; s_cur_slot = 1; return M_ISMP_FX; }
    if (i == KR_FX3) { s_setup_return = KR_FX3; s_cur_slot = 2; return M_ISMP_FX; }
    if (i == KR_SAVE) {                  // Save Patch: mint + write, stay on Setup
        if (keys_patch_save(s_last_saved, sizeof(s_last_saved)) != 0)
            snprintf(s_last_saved, sizeof(s_last_saved), "err");
        return 0;                        // framework redraws -> row shows the id
    }
    if (i == KR_LOADPAT) return M_ISMP_PATCH;    // Load Patch -> patch browser
    return 0;
}

static setup_menu_t ks_setup = {
    .items = ks_setup_items,
    .n = KR_N,
    .title = "Keys Setup",
    .aff_label = "Machine", .aff_target = M_MORE,
    .live_target = M_ISMP_LIVE,
    .render = ks_val, .adjust = ks_adj, .action = ks_action,
};

// ---- FX sub-page: all five effects, reached from the Setup "FX" row --------
// ===== FX rack: the FX sub-page edits ONE slot (s_cur_slot); rows/params come
// from the shared fxrack module. FX1/FX2 generic, FX3 = reverb. ==============
#define KFX_MAXROWS FXRACK_MAXROWS
static setup_item_t s_kfx_items[KFX_MAXROWS];
static int8_t s_kfx_param[KFX_MAXROWS];
static int s_kfx_n;
static setup_menu_t ks_fx;   // defined below

static void kfx_rebuild(void){
    s_kfx_n = fxrack_menu_rows(&inst_rk, s_cur_slot, s_kfx_items, s_kfx_param);
    ks_fx.n = s_kfx_n;
    ks_fx.title = s_cur_slot==0?"Keys FX1":s_cur_slot==1?"Keys FX2":"Keys FX3";
    if (ks_fx.sel >= s_kfx_n) ks_fx.sel = s_kfx_n-1;
    if (ks_fx.sel < 0) ks_fx.sel = 0;
}

static void ks_fx_val(int i, char *v, size_t n){
    if (i < 0 || i >= s_kfx_n) { v[0] = 0; return; }
    fxrack_menu_val(&inst_rk, s_cur_slot, s_kfx_param[i], v, n);
}

static void ks_fx_adj(int i, int dir){
    if (i < 0 || i >= s_kfx_n) return;
    int p = s_kfx_param[i];
    fxrack_menu_adj(&inst_rk, s_cur_slot, p, dir);
    if (p < 0) kfx_rebuild();   // effect changed -> param rows changed
}

static setup_menu_t ks_fx = {
    .items = s_kfx_items,
    .n = 0,                       // set by kfx_rebuild()
    .title = "Keys FX",           // overwritten per-slot by kfx_rebuild()
    .aff_label = "Setup", .aff_target = M_ISMP_SETUP,
    .live_target = M_ISMP_SETUP,  // long-press = up one level to Setup
    .render = ks_fx_val, .adjust = ks_fx_adj, .action = NULL,
};

static int keys_fx_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) kfx_rebuild();   // dynamic row set
    return setup_menu_event(&ks_fx, event);
}

static int keys_setup_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU && s_setup_return >= 0) {   // came back from a sub-page
        int p = s_setup_return; s_setup_return = -1;
        setup_menu_enter_at(&ks_setup, p);                   // land on the line we left
        return 0;
    }
    return setup_menu_event(&ks_setup, event);
}

// ---- Load Sample / Add Zone: the shared sample browser --------------------
static int keys_load_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) {
        // plain browse (opens in POOL, remembers position, all folders reachable).
        // NOT forced into usr/KEYS: that folder starts empty, and landing there
        // leaves nothing to load. KEYS is still a folder row + web destination.
        sample_browser_enter(true, s_browse_append ? "Add Zone" : "Load Sample",
                             ks_z()->sample);
        return 0;
    }
    int r = sample_browser_event(event);
    if (r == 1) {
        // APPEND builds the multisample; a plain load REPLACES the whole set, so
        // picking a sample from the browser can never quietly turn a one-shot
        // instrument into a multisample (or silently discard one).
        if (s_browse_append) {
            s_browse_append = false;
            if (keys_load_zone_at(-1, sample_browser_selected()) != 0)
                return M_ISMP_SETUP;    // load_err is on the Setup page
            keys_build_peaks();
            return M_ISMP_SETUP;        // stay put: you are usually adding several
        }
        keys_load_zone(sample_browser_selected());
        keys_build_peaks();
        return M_ISMP_LIVE;             // loaded: jump straight to Live to play it
    }
    if (r == 2) { s_browse_append = false; return M_ISMP_SETUP; }   // cancelled
    return 0;
}

// ---- CV Matrix page: the shared cvmtx widget drives everything --------------
static int keys_matrix_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    return cvmtx_menu_event(&inst.mtx, event, "Keys CV Matrix",
                            M_ISMP_SETUP, M_ISMP_LIVE);
}

// ---- Load Patch: a scrollable list of usr/keys/PAT_NNN.jsn (newest first) ---
// The Synth #23 browser page verbatim (over the shared preset_store).
#define KS_PATCH_MAX 64
static char s_patches[KS_PATCH_MAX][12];
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

static int keys_patch_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU:
            s_npatch = keys_patch_list(s_patches, KS_PATCH_MAX);
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
            if (s_patch_sel < 0) return M_ISMP_SETUP;           // affordance / empty
            keys_patch_load(s_patches[s_patch_sel]);
            return M_ISMP_SETUP;
        case EV_LONG_PRESS: return M_ISMP_SETUP;
        default: break;
    }
    return 0;
}

static void keys_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_ISMP_LIVE);   menusys_item_set_default_cb(_ms, M_ISMP_LIVE, keys_live_handler);
    menusys_new_item(_ms, M_ISMP_SETUP);  menusys_item_set_default_cb(_ms, M_ISMP_SETUP, keys_setup_handler);
    menusys_new_item(_ms, M_ISMP_LOAD);   menusys_item_set_default_cb(_ms, M_ISMP_LOAD, keys_load_handler);
    menusys_new_item(_ms, M_ISMP_MATRIX); menusys_item_set_default_cb(_ms, M_ISMP_MATRIX, keys_matrix_handler);
    menusys_new_item(_ms, M_ISMP_PATCH);  menusys_item_set_default_cb(_ms, M_ISMP_PATCH, keys_patch_handler);
    menusys_new_item(_ms, M_ISMP_FX);     menusys_item_set_default_cb(_ms, M_ISMP_FX, keys_fx_handler);
}

static int keys_main_event(int event, void *ev_data)
{
    (void)ev_data;
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW) {
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        char nm[12]; note_name_midi((int)lroundf(inst.note_disp), nm, sizeof(nm));
        _fg = TFT_LIGHTGREY;
        char s[64]; snprintf(s, sizeof(s), "Keys: %s  %s", nm, ks_z()->sample[0] ? ks_z()->sample : "(no sample)");
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const keys_main_items[] = { "Live", "Setup", "Matrix" };
static const int keys_main_targets[] = { M_ISMP_LIVE, M_ISMP_SETUP, M_ISMP_MATRIX };

const machine_ui_t keys_menu_ui = {
    .main_items = keys_main_items,
    .main_targets = keys_main_targets,
    .n_main = 3,
    .register_pages = keys_register_pages,
    .main_event = keys_main_event,
    .boot_target = M_ISMP_LIVE,
};
