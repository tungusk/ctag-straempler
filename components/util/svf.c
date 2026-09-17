#include <math.h>
#include "svf.h"

float svf_coef(float fc, float sr, float fmax)
{
    if (fc <= 0.0f) return 0.0f;
    // each caller's old Chamberlin ceiling, as the frequency it used to reach
    if (fmax < 2.0f) {
        float fc_max = sr * asinf(fmax * 0.5f) / 3.14159265f;
        if (fc > fc_max) fc = fc_max;
    }
    if (fc > 0.45f * sr) fc = 0.45f * sr;          // tan() runs away at Nyquist
    return tanf(3.14159265f * fc / sr);
}

float svf_damp(float res01, float qmin, float qmax)
{
    if (res01 < 0.0f) res01 = 0.0f;
    if (res01 > 1.0f) res01 = 1.0f;
    return qmax - res01 * (qmax - qmin);   // knob up = less damping = more resonance
}
