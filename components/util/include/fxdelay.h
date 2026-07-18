#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Shared clock-syncable DELAY (the FX-rack brick after svf/reverb, 2026-07-17).
// One stereo delay line carved from a PSRAM slab; feedback with a damping
// one-pole (analog-style darkening repeats), equal-power wet/dry, optional
// ping-pong (feedback crosses L<->R). Hosts drop it into their output stage
// exactly like reverb: `fxdelay_block_i32` processes the machine's interleaved
// int32<<16 buffer IN PLACE.
//
// Named `fxdelay` (not `delay`) because machine_sampler2 already ships a legacy,
// incompatible `delay_t` in its own delay.h — the two must not collide.
//
// Time can be set two ways: `fxdelay_set_time_ms` (free) or
// `fxdelay_set_time_beats` against a BPM (clock-synced — for machines that know
// a tempo). All setters are UI-task-safe: they write plain values the audio
// kernel reads per block. The line contents survive a time change (repeats just
// re-tap at the new offset — the classic delay "pitch jump", which is musical),
// so no realloc, no lock.
//
// Ownership: `fxdelay_init` allocates the slab and leaves the effect silent
// (wet 0). Returns ESP_ERR_NO_MEM (leaving `d` unusable) on a failed alloc.

#define FXD_RATE      44100
#define FXD_MAX_SEC   2.0f                       // slab capacity; longer clamps
#define FXD_MAX_FR    ((int)(FXD_RATE * FXD_MAX_SEC))

typedef struct {
    float *bufL, *bufR;         // PSRAM, one slab (bufR = bufL + cap)
    int    cap;                 // frames of capacity (== FXD_MAX_FR)
    int    w;                   // write cursor
    volatile int   len;         // live delay length in frames (1..cap-1)
    volatile float fb;          // feedback gain, 0..0.95
    volatile float wet;         // 0..1, equal-power mix
    volatile bool  pingpong;    // cross the feedback taps L<->R
    volatile float damp;        // feedback damping, 0 = bright .. 1 = dark
    float  lpL, lpR;            // feedback one-pole states
    volatile int cost_us;       // EMA process cost, us per block (1450 = 100%)
} fxdelay_t;

esp_err_t fxdelay_init(fxdelay_t *d);   // alloc slab, silent (len 375ms, wet 0)
void fxdelay_free(fxdelay_t *d);
void fxdelay_clear(fxdelay_t *d);       // flush the line + damping state

void fxdelay_set_time_ms(fxdelay_t *d, float ms);              // free time
void fxdelay_set_time_beats(fxdelay_t *d, float beats, float bpm); // clock-synced
void fxdelay_set_feedback(fxdelay_t *d, float fb);   // clamped 0..0.95
void fxdelay_set_mix(fxdelay_t *d, float wet);       // 0..1
void fxdelay_set_damp(fxdelay_t *d, float d01);      // 0 bright .. 1 dark
void fxdelay_set_pingpong(fxdelay_t *d, bool on);

// Process one machine block IN PLACE on the interleaved int32<<16 output
// (`frames` stereo frames). No-op if `d` is unallocated. The caller decides
// whether the effect is engaged; when disengaged it should stop calling this
// (and may fxdelay_clear to drop the tail) rather than pass wet 0, since a live
// feedback tail must keep circulating to ring out.
void fxdelay_block_i32(fxdelay_t *d, int32_t *out, int frames);
// float-scratch worker for the hosted chain (see fxchain.h)
void fxdelay_block_f(fxdelay_t *d, float *buf, int frames);

static inline float fxdelay_time_ms(const fxdelay_t *d)
{
    return d->cap ? (float)d->len * 1000.0f / (float)FXD_RATE : 0.0f;
}
