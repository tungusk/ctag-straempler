// Filter brick — see fxfilter.h. Multimode insert filter over the shared svf.
#include <math.h>
#include "fxfilter.h"

#define FLT_RATE 44100.0f

void fxfilter_block_f(fxfilter_t *fl, float *buf, int frames)
{
    int   mode = fl->mode; if (mode < 0 || mode >= FILT_NMODE) mode = FILT_LP;
    float cut  = fl->cutoff; if (cut < 0) cut = 0; else if (cut > 1) cut = 1;
    float reso = fl->reso;   if (reso < 0) reso = 0; else if (reso > 1) reso = 1;

    float fc = 30.0f * powf(400.0f, cut);          // 30 Hz .. ~12 kHz (log)
    float cf = svf_coef(fc, FLT_RATE, 1.3f);
    // q is DAMPING (higher = cleaner). reso 0 -> 2.0 (clean), 1 -> 0.3 (resonant);
    // floor at 0.3 keeps the filter stable (no NaN self-osc runaway).
    float q  = 2.0f - reso * 1.7f;

    for (int f = 0; f < frames; f++) {
        fl->cf_slew += 0.05f * (cf - fl->cf_slew);  // de-zipper the sweep
        float xl = buf[f * 2], xr = buf[f * 2 + 1];
        float lpl, bpl, hpl, lpr, bpr, hpr;
        svf_step(&fl->l, xl, fl->cf_slew, q, &lpl, &bpl, &hpl);
        svf_step(&fl->r, xr, fl->cf_slew, q, &lpr, &bpr, &hpr);
        buf[f * 2]     = mode == FILT_LP ? lpl : mode == FILT_HP ? hpl : bpl;
        buf[f * 2 + 1] = mode == FILT_LP ? lpr : mode == FILT_HP ? hpr : bpr;
    }
}
