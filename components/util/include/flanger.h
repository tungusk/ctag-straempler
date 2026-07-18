#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Shared FLANGER (FX-rack brick, 2026-07-17). A short LFO-swept delay
// (~1..11 ms) with feedback and equal-power wet/dry — the classic jet sweep.
// A small stereo line lives in a PSRAM slab (lazy). Rate is free (Hz) or
// clock-synced (flanger_set_rate_beats: one sweep per N beats). Fractional
// (linearly interpolated) read for a smooth sweep. Process IN PLACE on the
// interleaved int32<<16 stereo buffer.
//
// Ownership like fxdelay: flanger_init allocates the slab and leaves it silent
// (wet 0); returns ESP_ERR_NO_MEM (leaving `g` unusable) on a failed alloc.

#define FLG_RATE    44100
#define FLG_MAX_MS  12.0f
#define FLG_MAX_FR  ((int)(FLG_RATE * FLG_MAX_MS / 1000.0f))

typedef struct {
    float *bufL, *bufR;     // PSRAM, one slab (bufR = bufL + cap)
    int    cap, w;
    volatile float rate;    // Hz LFO (0.01..10)
    volatile float depth;   // 0..1 sweep depth
    volatile float fb;      // -0.95..0.95 feedback
    volatile float wet;     // 0..1 equal-power mix
    float  phase;           // 0..1 LFO phase
    float  lpL, lpR;        // feedback damping one-pole state (tames the ring-up)
} flanger_t;

static inline void flanger_set_rate_beats(flanger_t *g, float beats, float bpm)
{
    if (bpm < 1.0f) bpm = 1.0f;
    if (beats < 0.01f) beats = 0.01f;
    g->rate = bpm / (60.0f * beats);
}

esp_err_t flanger_init(flanger_t *g);   // alloc slab, silent (wet 0)
void flanger_free(flanger_t *g);
void flanger_clear(flanger_t *g);       // flush the line

void flanger_block_i32(flanger_t *g, int32_t *out, int frames);
// float-scratch worker for the hosted chain (see fxchain.h)
void flanger_block_f(flanger_t *g, float *buf, int frames);
