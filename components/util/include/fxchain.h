#pragma once
#include <stdint.h>

// Gain-staging for the hosted multi-FX chain (Keys / Synth / Tape). Each effect
// exposes a float worker (*_block_f) that reads AND writes a stereo-interleaved
// float scratch buffer WITHOUT clamping. The host unpacks the int32<<16 machine
// buffer to float ONCE, runs the enabled float stages back-to-back, then
// soft-clips + packs ONCE at the end.
//
// Why: the old chain called only the *_block_i32 entry points, each of which
// hard-clamped to int16 and round-tripped through the machine buffer between
// stages. That left ZERO internal headroom — correlated wet sums (flanger/delay
// combs peak at +3 dB when wet lines up with dry, a built-up reverb tail) got
// flat-topped at EVERY stage and stacked into harsh clipping. Running the middle
// of the chain in float, with a single soft limiter at the end, removes the
// inter-stage clipping and makes the final ceiling graceful instead of a
// flat-top. The *_block_i32 wrappers stay for single-FX callers (Drums delay,
// Slicer reverb) — byte-for-byte the old behaviour.
//
// buf: stereo-interleaved float, sample units (±32768 nominal; MAY exceed
// between stages — that headroom is the whole point). n2 = frames * 2.

// scratch size for the *_block_i32 wrappers: MACHINE_BLOCK (64 = 32 frames * 2).
// Every machine calls with frames = MACHINE_BLOCK/2, so 64 interleaved samples.
#define FX_SCRATCH_N 64

static inline void fx_unpack_i32(const int32_t *out, float *buf, int n2)
{
    for (int i = 0; i < n2; i++) buf[i] = (float)(out[i] >> 16);
}

// Soft limiter: EXACTLY linear below the knee (normal levels pass untouched),
// smooth saturating approach to ±full-scale above it. Replaces the per-stage
// hard clamp so the ceiling bends instead of flat-topping. Cheap (no libm) and
// C1-continuous (slope 1 at the knee), so it doesn't color in-range signal.
static inline float fx_softclip1(float x)
{
    const float lim = 32767.0f, kn = 0.70f * lim;   // linear below ~-3 dBFS
    float a = x < 0.0f ? -x : x;
    if (a <= kn) return x;
    float over = (a - kn) / (lim - kn);             // 0 at knee, ->inf beyond
    float y = kn + (lim - kn) * (over / (1.0f + over));  // asymptotes to lim
    return x < 0.0f ? -y : y;
}

static inline void fx_pack_softclip(const float *buf, int32_t *out, int n2)
{
    for (int i = 0; i < n2; i++) {
        int32_t s = (int32_t)fx_softclip1(buf[i]);
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;   // paranoia
        out[i] = s << 16;
    }
}
