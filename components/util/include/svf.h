#pragma once
#include <stdbool.h>

// Chamberlin state-variable filter — the one DSP kernel three machines had each
// written out for themselves (deck's DJ filter, the looper's per-track bandpass,
// and the looper AGAIN in its bounce path, which has to bake audio identical to
// the engine). Drums would have been the fourth copy.
//
// `q` here is DAMPING, not Q: HIGHER IS CLEANER. The looper sweeps 2.0 -> 0.1;
// the deck nails it at 0.9. Resonance is 1/q, roughly.
//
// What this util deliberately does NOT own:
//   - the coefficient SLEW. The deck slews f per block (no zipper on a fast
//     sweep); the looper recomputes hard and relies on the CV being slow. That's
//     a per-machine choice, so callers keep it.
//   - the clamp ceiling. The deck's 1.2 is not a tidy-up candidate: at 12 kHz
//     the unclamped coefficient is ~1.51, so 1.2 IS the top of its LP sweep.
//     Passing it as `fmax` keeps every existing sweep bit-identical.

typedef struct { float lp, bp; } svf_t;

// One sample through the filter. All three taps are available; pass NULL for the
// ones you don't want (the compiler drops them). The operation ORDER is the
// contract — deck and looper audio is bit-compared against it, so do not
// "simplify" this into a different arrangement of the same algebra.
static inline void svf_step(svf_t *s, float x, float f, float q,
                            float *lp, float *bp, float *hp)
{
    s->lp += f * s->bp;
    float hi = x - s->lp - q * s->bp;
    s->bp += f * hi;
    if (lp) *lp = s->lp;
    if (bp) *bp = s->bp;
    if (hp) *hp = hi;
}

// Bypass without a thump: park the state ON the signal, so when the filter is
// re-engaged its memory already agrees with what the ear is hearing (the deck's
// trick — a filter whose state is stale by a whole waveform lets out a click).
static inline void svf_park(svf_t *s, float x) { s->lp = x; s->bp = 0.0f; }

static inline void svf_reset(svf_t *s) { s->lp = 0.0f; s->bp = 0.0f; }

// Per-block helpers (a sinf or two per block is free; per SAMPLE it would not be)
float svf_coef(float fc, float sr, float fmax);        // 2*sin(pi*fc/sr), clamped to fmax
float svf_damp(float res01, float qmin, float qmax);   // 0..1 knob -> damping (qmax = clean)
