#pragma once
#include <stdint.h>
#include "svf.h"

// Filter brick for the FX rack — a standalone multimode filter (LP/HP/BP) as an
// INSERT effect (distinct from the env-modulated VOICE filter in Synth/Keys).
// A trapezoidal ("zero-delay feedback") SVF, NOT the shared Chamberlin svf: the
// Chamberlin goes unstable toward the top of this effect's 12 kHz range, and
// just below the bound it grows a +8..13 dB peak at Nyquist that the rack's
// soft limiter turned into a blow-up (code review 2.1; Arlo heard it 09-16 even
// with the coefficient clamped). The trapezoidal form is stable at any cutoff
// and keeps the same response shape across the whole sweep. Stereo, float-scratch
// worker matching the fxchain.h convention (see fxrack). A base/width band
// filter is a planned second flavor (plans/fx-rack-20260717.md).

enum { FILT_LP = 0, FILT_HP, FILT_BP, FILT_NMODE };

typedef struct {
    volatile int   mode;     // FILT_LP / FILT_HP / FILT_BP
    volatile float cutoff;   // 0..1 -> ~30 Hz .. 12 kHz (log)
    volatile float reso;     // 0..1 -> resonance (0 clean .. 1 near self-osc)
    float  ic1[2], ic2[2];   // per-channel integrator states (L, R)
    float  cf_slew;          // slewed g = tan(pi*fc/sr) (no zipper on a fast sweep)
} fxfilter_t;

static inline void fxfilter_init(fxfilter_t *fl)
{
    fl->mode = FILT_LP; fl->cutoff = 0.6f; fl->reso = 0.2f;
    fl->ic1[0] = fl->ic1[1] = fl->ic2[0] = fl->ic2[1] = 0.0f; fl->cf_slew = 0.0f;
}

// float-scratch worker (no clamp; the rack soft-limits at the end)
void fxfilter_block_f(fxfilter_t *fl, float *buf, int frames);

// BAND filter: a bandpass parameterized by center ("base", reuses .cutoff) and
// bandwidth ("width", reuses .reso: 0 narrow/resonant .. 1 wide/gentle). Own
// fxfilter_t instance so it can coexist with the LP/HP/BP brick in another slot.
void fxfilter_band_block_f(fxfilter_t *fl, float *buf, int frames);
