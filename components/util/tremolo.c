// Shared tremolo / auto-pan — see tremolo.h.
//
// gain = 1 - depth*(0.5 - 0.5*wave), so the amplitude rides between (1-depth)
// and 1 (never above unity -> no clipping). In stereo mode the right channel
// reads the LFO half a cycle out of phase, which turns level modulation into
// left/right auto-pan.

#include <math.h>
#include "tremolo.h"

#define TR_RATE 44100.0f

static inline float trem_wave(int shape, float ph)
{
    switch (shape) {
        case TREM_SQR: return ph < 0.5f ? 1.0f : -1.0f;
        case TREM_TRI: return ph < 0.5f ? (4.0f * ph - 1.0f) : (3.0f - 4.0f * ph);
        default:       return sinf(6.2831853f * ph);
    }
}

void tremolo_block_i32(tremolo_t *t, int32_t *out, int frames)
{
    float rate  = t->rate;  if (rate < 0.01f) rate = 0.01f; else if (rate > 30.0f) rate = 30.0f;
    float depth = t->depth; if (depth < 0) depth = 0; else if (depth > 1) depth = 1;
    int   shape = t->shape; if (shape < 0 || shape >= TREM_NSHAPE) shape = TREM_SINE;
    bool  st    = t->stereo;
    float inc   = rate / TR_RATE;
    float ph    = t->phase;

    for (int f = 0; f < frames; f++) {
        float gl = 1.0f - depth * (0.5f - 0.5f * trem_wave(shape, ph));
        float gr = gl;
        if (st) {
            float pr = ph + 0.5f; if (pr >= 1.0f) pr -= 1.0f;
            gr = 1.0f - depth * (0.5f - 0.5f * trem_wave(shape, pr));
        }
        out[f * 2]     = ((int32_t)((float)(out[f * 2]     >> 16) * gl)) << 16;
        out[f * 2 + 1] = ((int32_t)((float)(out[f * 2 + 1] >> 16) * gr)) << 16;
        ph += inc; if (ph >= 1.0f) ph -= 1.0f;
    }
    t->phase = ph;
}
