#pragma once
#include <stdint.h>
#include "svf.h"

// Filter brick for the FX rack — a standalone multimode filter (LP/HP/BP) as an
// INSERT effect (distinct from the env-modulated VOICE filter in Synth/Keys).
// Wraps the shared Chamberlin svf (components/util/svf). Stereo, float-scratch
// worker matching the fxchain.h convention (see fxrack). A base/width band
// filter is a planned second flavor (plans/fx-rack-20260717.md).

enum { FILT_LP = 0, FILT_HP, FILT_BP, FILT_NMODE };

typedef struct {
    volatile int   mode;     // FILT_LP / FILT_HP / FILT_BP
    volatile float cutoff;   // 0..1 -> ~30 Hz .. 12 kHz (log)
    volatile float reso;     // 0..1 -> resonance (0 clean .. 1 near self-osc)
    svf_t  l, r;             // per-channel filter state
    float  cf_slew;          // slewed coefficient (no zipper on a fast sweep)
} fxfilter_t;

static inline void fxfilter_init(fxfilter_t *fl)
{
    fl->mode = FILT_LP; fl->cutoff = 0.6f; fl->reso = 0.2f;
    svf_reset(&fl->l); svf_reset(&fl->r); fl->cf_slew = 0.0f;
}

// float-scratch worker (no clamp; the rack soft-limits at the end)
void fxfilter_block_f(fxfilter_t *fl, float *buf, int frames);
