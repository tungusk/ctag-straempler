#pragma once
#include <math.h>
#include <stdbool.h>

// THE state-variable filter — every filter in the firmware: deck/DoubleDecker
// DJ filters, the looper's per-track bandpass (engine AND bounce), Slicer,
// Tracker, Drums, the Synth/Keys/Tape voice filters, the FX Filter/Band bricks,
// the overdrive tone and the beat listener's bands.
//
// TRAPEZOIDAL ("zero-delay feedback", Simper) since 2026-09-16. It was a
// Chamberlin until then, and the Chamberlin is only stable for
// f < sqrt(q*q + 4) - q: the Synth/Keys/Tape voice filters (fmax 1.0, damping up
// to 2.0) crossed that at low resonance and high cutoff and blew up — Arlo heard
// it below ~35% resonance, which is exactly where damping passes 1.5. Just under
// the bound it also peaked +8..13 dB at Nyquist. The trapezoidal form is stable
// for every cutoff below Nyquist and keeps one response shape across the sweep.
// Arlo's call: one filter everywhere. Deck/looper audio is NOT bit-identical to
// the pre-09-16 recordings any more (low and mid cutoffs match closely; the top
// of each sweep changes because it no longer misbehaves).
//
// `q` here is DAMPING, not Q: HIGHER IS CLEANER (the trapezoidal `k`). The
// looper sweeps 2.0 -> 0.1; the deck nails it at 0.9. Resonance is 1/q, roughly.
// `f` is the coefficient from svf_coef() — tan(pi*fc/sr) now, not 2*sin — so
// callers that slew f per block keep doing exactly that.
//
// What this util deliberately does NOT own:
//   - the coefficient SLEW (a per-machine choice, callers keep it).
//   - the top of each machine's sweep: svf_coef's `fmax` is still the caller's
//     old Chamberlin ceiling, converted to the same FREQUENCY cap (the deck's
//     1.2 still tops out at ~9 kHz), so every range stays where it was.

typedef struct { float lp, bp; } svf_t;   // the two integrator states (ic2, ic1)

// One sample through the filter. All three taps are available; pass NULL for the
// ones you don't want (the compiler drops them).
static inline void svf_step(svf_t *s, float x, float f, float q,
                            float *lp, float *bp, float *hp)
{
    float a1 = 1.0f / (1.0f + f * (f + q));
    float a2 = f * a1, a3 = f * a2;
    float v3 = x - s->lp;
    float v1 = a1 * s->bp + a2 * v3;               // band
    float v2 = s->lp + a2 * s->bp + a3 * v3;       // low
    s->bp = 2.0f * v1 - s->bp;
    s->lp = 2.0f * v2 - s->lp;
    if (lp) *lp = v2;
    if (bp) *bp = v1;
    if (hp) *hp = x - q * v1 - v2;
}

// Bypass without a thump: park the state ON the signal, so when the filter is
// re-engaged its memory already agrees with what the ear is hearing (the deck's
// trick — a filter whose state is stale by a whole waveform lets out a click).
static inline void svf_park(svf_t *s, float x) { s->lp = x; s->bp = 0.0f; }

static inline void svf_reset(svf_t *s) { s->lp = 0.0f; s->bp = 0.0f; }

// Per-block helpers (a sinf or two per block is free; per SAMPLE it would not be)
float svf_coef(float fc, float sr, float fmax);        // tan(pi*fc/sr); fc capped where 2*sin(pi*fc/sr) = fmax (the old ceiling)
float svf_damp(float res01, float qmin, float qmax);   // 0..1 knob -> damping (qmax = clean)
