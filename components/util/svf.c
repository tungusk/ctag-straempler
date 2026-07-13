#include <math.h>
#include "svf.h"

float svf_coef(float fc, float sr, float fmax)
{
    if (fc <= 0.0f) return 0.0f;
    float f = 2.0f * sinf(3.14159265f * fc / sr);
    if (f > fmax) f = fmax;       // the Chamberlin goes unstable as f -> 2
    if (f < 0.0f) f = 0.0f;
    return f;
}

float svf_damp(float res01, float qmin, float qmax)
{
    if (res01 < 0.0f) res01 = 0.0f;
    if (res01 > 1.0f) res01 = 1.0f;
    return qmax - res01 * (qmax - qmin);   // knob up = less damping = more resonance
}
