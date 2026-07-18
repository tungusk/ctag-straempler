#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "svf.h"

// Shared OVERDRIVE / saturation (FX-rack brick, 2026-07-17). A tanh waveshaper
// with drive, a post-shape tone tilt (svf low-pass), asymmetry/bias (adds even
// harmonics, DC-compensated), and an output trim. Stateless except the tone
// filter, so there is NO PSRAM slab — zero-init the struct (or overdrive_reset
// to clear the filter). The host writes the param fields directly (the menu
// clamps); the kernel re-clamps defensively. Process IN PLACE on the machine's
// interleaved int32<<16 stereo buffer.

typedef struct {
    volatile float drive;   // 0..1 -> input gain 1..~30
    volatile float tone;    // 0..1 post low-pass tilt (0 dark .. 1 bright)
    volatile float bias;    // -1..1 asymmetry (0 = symmetric)
    volatile float level;   // 0..1 output trim
    svf_t  tl, tr;          // tone filter state per channel
} overdrive_t;

static inline void overdrive_reset(overdrive_t *o) { svf_reset(&o->tl); svf_reset(&o->tr); }

void overdrive_block_i32(overdrive_t *o, int32_t *out, int frames);
