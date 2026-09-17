// Filter brick — see fxfilter.h. Multimode insert filter, trapezoidal SVF
// (Simper's "zero-delay feedback" state-variable form): stable for any cutoff
// below Nyquist, no Nyquist gain peak, same damping parameter as the old svf.
#include <math.h>
#include "fxfilter.h"

#define FLT_RATE 44100.0f

static inline float g_of(float fc)
{
    if (fc > 0.45f * FLT_RATE) fc = 0.45f * FLT_RATE;
    return tanf(3.14159265f * fc / FLT_RATE);
}

// one sample through channel c; k = damping (2 clean .. 0.3 resonant)
static inline void tpt_step(fxfilter_t *fl, int c, float v0, float g, float k,
                            float *lp, float *bp, float *hp)
{
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1, a3 = g * a2;
    float v3 = v0 - fl->ic2[c];
    float v1 = a1 * fl->ic1[c] + a2 * v3;
    float v2 = fl->ic2[c] + a2 * fl->ic1[c] + a3 * v3;
    fl->ic1[c] = 2.0f * v1 - fl->ic1[c];
    fl->ic2[c] = 2.0f * v2 - fl->ic2[c];
    *lp = v2; *bp = v1; *hp = v0 - k * v1 - v2;
}

// stable by construction; this only catches a NaN fed in from upstream
static inline void guard(fxfilter_t *fl, float g)
{
    for (int c = 0; c < 2; c++)
        if (!(fabsf(fl->ic1[c]) < 1e6f) || !(fabsf(fl->ic2[c]) < 1e6f)) {
            fl->ic1[0] = fl->ic1[1] = fl->ic2[0] = fl->ic2[1] = 0.0f;
            fl->cf_slew = g;
            return;
        }
    if (!(fl->cf_slew >= 0.0f && fl->cf_slew < 20.0f)) fl->cf_slew = g;
}

void fxfilter_block_f(fxfilter_t *fl, float *buf, int frames)
{
    int   mode = fl->mode; if (mode < 0 || mode >= FILT_NMODE) mode = FILT_LP;
    float cut  = fl->cutoff; if (cut < 0) cut = 0; else if (cut > 1) cut = 1;
    float reso = fl->reso;   if (reso < 0) reso = 0; else if (reso > 1) reso = 1;

    float fc = 30.0f * powf(400.0f, cut);          // 30 Hz .. ~12 kHz (log)
    float g  = g_of(fc);
    // k is DAMPING (higher = cleaner). reso 0 -> 2.0 (clean), 1 -> 0.3 (resonant)
    float k  = 2.0f - reso * 1.7f;
    guard(fl, g);

    for (int f = 0; f < frames; f++) {
        fl->cf_slew += 0.05f * (g - fl->cf_slew);  // de-zipper the sweep
        float lpl, bpl, hpl, lpr, bpr, hpr;
        tpt_step(fl, 0, buf[f * 2],     fl->cf_slew, k, &lpl, &bpl, &hpl);
        tpt_step(fl, 1, buf[f * 2 + 1], fl->cf_slew, k, &lpr, &bpr, &hpr);
        buf[f * 2]     = mode == FILT_LP ? lpl : mode == FILT_HP ? hpl : bpl;
        buf[f * 2 + 1] = mode == FILT_LP ? lpr : mode == FILT_HP ? hpr : bpr;
    }
}

void fxfilter_band_block_f(fxfilter_t *fl, float *buf, int frames)
{
    float base  = fl->cutoff; if (base < 0) base = 0; else if (base > 1) base = 1;
    float width = fl->reso;   if (width < 0) width = 0; else if (width > 1) width = 1;
    float fc = 30.0f * powf(400.0f, base);            // center: 30 Hz .. ~12 kHz
    float g  = g_of(fc);
    float k  = 0.3f + width * 3.7f;                   // narrow/resonant .. wide/gentle
    guard(fl, g);
    for (int f = 0; f < frames; f++) {
        fl->cf_slew += 0.05f * (g - fl->cf_slew);
        float lpl, bpl, hpl, lpr, bpr, hpr;
        tpt_step(fl, 0, buf[f * 2],     fl->cf_slew, k, &lpl, &bpl, &hpl);
        tpt_step(fl, 1, buf[f * 2 + 1], fl->cf_slew, k, &lpr, &bpr, &hpr);
        buf[f * 2]     = bpl;                          // bandpass tap
        buf[f * 2 + 1] = bpr;
    }
}
