#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Shared multi-mode REVERB (the FX-rack brick after svf, 2026-07-13).
// A Dattorro-style plate tank (figure-8, cross-coupled branches) whose modes
// are parameter sets over ONE core — plus a pitch-shifted feedback path that
// turns the Hall into a SHIMMER (octave-up tap inside the tail).
//
//   RV_ROOM     small, darker, fast decay
//   RV_HALL     full size, medium decay
//   RV_PLATE    bright, dense, classic plate sheen
//   RV_SHIMMER  hall + octave-up feedback blooming upward
//
// Ownership: all delay lines live in ONE PSRAM slab (~170 KB float),
// allocated by reverb_init and touched only from the audio task via
// reverb_block_i32. Hosts drop it into their output stage AFTER the filter;
// wet/dry is equal-power and performable. reverb_set_* are UI-task-safe
// (params are plain floats the kernel reads per block).

typedef enum {
    RV_OFF = 0,
    RV_ROOM,
    RV_HALL,
    RV_PLATE,
    RV_SHIMMER,
    RV_N_MODES
} rv_mode_t;

// one delay line inside the slab: base capacity, live length (size-scaled)
typedef struct { float *buf; int cap, len, w; } rv_line_t;

typedef struct {
    float *slab;                // PSRAM, all lines carved from here
    // input conditioning
    rv_line_t pre;              // predelay
    float in_lp;                // input bandwidth one-pole state
    rv_line_t ap_in[4];         // input diffusion allpasses
    // tank, two cross-coupled branches
    rv_line_t ap_a1, d_a1, ap_a2, d_a2;
    rv_line_t ap_b1, d_b1, ap_b2, d_b2;
    float damp_a, damp_b;       // damping LPF states
    // shimmer: octave-up dual-head granular on the tank feed
    rv_line_t shim;             // shifter window
    float shim_pos;             // read phase (two heads at +N/2)
    // params (mode presets; UI-task-safe writes)
    volatile int   mode;
    float decay;                // tank feedback gain
    float damp;                 // damping coefficient (0 = bright)
    float in_bw;                // input bandwidth
    float shim_gain;            // pitch-feedback amount (0 = none)
    volatile float wet;         // 0..1 mix, equal-power
    // instrumentation: EMA of process cost, microseconds per block
    volatile int cost_us;
} reverb_t;

// allocate the PSRAM slab and set RV_OFF. Safe to call from UI tasks.
// Returns ESP_ERR_NO_MEM (and leaves rv unusable) on a failed alloc.
esp_err_t reverb_init(reverb_t *rv);
void reverb_free(reverb_t *rv);

void reverb_set_mode(reverb_t *rv, int mode);   // applies the mode's preset
void reverb_set_mix(reverb_t *rv, float wet);   // 0..1

// process one machine block IN PLACE on the interleaved int32<<16 output
// buffer (the machine format). No-op when mode == RV_OFF or rv is unallocated.
void reverb_block_i32(reverb_t *rv, int32_t *out, int frames);

static inline const char *reverb_mode_name(int m)
{
    switch (m) {
        case RV_ROOM:    return "Room";
        case RV_HALL:    return "Hall";
        case RV_PLATE:   return "Plate";
        case RV_SHIMMER: return "Shimmer";
        default:         return "OFF";
    }
}
