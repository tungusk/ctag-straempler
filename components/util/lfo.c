// Shared sub-audio LFO (see lfo.h). Lifted out of the Synth on 2026-09-10 when
// Keys wanted the same section; the Synth's behaviour is preserved exactly.
#include <math.h>
#include "lfo.h"

const float lfo_beats[LFO_DIV_N] = { 16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f };

static int clampi(int v, int n) { return v < 0 ? 0 : (v >= n ? n - 1 : v); }

const char *lfo_div_name(int d)
{
    static const char *n[LFO_DIV_N] = { "4 bar", "2 bar", "1 bar", "1/2", "1/4", "1/8", "1/16" };
    return n[clampi(d, LFO_DIV_N)];
}

const char *lfo_shape_name(int s)
{
    static const char *n[LFO_SHAPE_N] = { "sine", "tri", "saw", "sqr", "rnd" };
    return n[clampi(s, LFO_SHAPE_N)];
}

// one LFO sample from a shape + phase. RND is sample-and-hold: it takes a new
// value when the phase wraps, so its step rate follows the rate/division too.
static float lfo_val(int shape, float ph, bool wrapped, float *rnd)
{
    switch (shape) {
        case LFO_TRI: return 1.0f - 4.0f * fabsf(ph - 0.5f);
        case LFO_SAW: return 2.0f * ph - 1.0f;
        case LFO_SQR: return ph < 0.5f ? 1.0f : -1.0f;
        case LFO_RND: {
            // local LCG rather than esp_random(): no header dependency (IDF 4.3
            // has none) and no syscall in the audio block
            static uint32_t lcg = 0x2545F491u;
            if (wrapped) { lcg = lcg * 1664525u + 1013904223u; *rnd = (float)(lcg >> 8) / 8388608.0f - 1.0f; }
            return *rnd;
        }
        default:      return sinf(6.2831853f * ph);
    }
}

float lfo_tick(lfo_t *l, int shape, float rate_hz, bool sync, int div,
               float bpm, uint32_t pulses, float ppb, float blockdur)
{
    // SYNC: the rate comes from the core clock's beat tempo, so the cycle is a
    // musical division rather than a free Hz. The phase is also re-zeroed at
    // each cycle boundary (counted in accepted pulses) — the rate is already
    // correct, so that correction is tiny and it keeps the LFO on the beat
    // instead of drifting to an arbitrary offset.
    float rate = rate_hz;
    if (sync && bpm > 0.0f) {
        float beats = lfo_beats[clampi(div, LFO_DIV_N)];
        rate = bpm / 60.0f / beats;
        if (ppb < 1.0f) ppb = 1.0f;
        uint32_t per = (uint32_t)(beats * ppb + 0.5f); if (!per) per = 1;
        uint32_t cyc = pulses / per;
        if (cyc != l->cyc) { l->cyc = cyc; l->phase = 0.0f; }
    }
    float ph_prev = l->phase;
    l->phase += rate * blockdur;
    bool wrapped = (l->phase >= 1.0f) || (l->phase < ph_prev);
    l->phase -= (float)(int)l->phase;
    return lfo_val(shape, l->phase, wrapped, &l->rnd);
}
