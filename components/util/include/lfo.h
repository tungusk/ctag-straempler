#pragma once
#include <stdint.h>
#include <stdbool.h>

// One shared sub-audio LFO: the shapes, the clock divisions and the per-block
// tick. A machine owns only the DESTINATION routing (what the value modulates)
// and its preset keys — everything below is identical wherever an LFO turns up,
// so it lives here instead of being copied per machine. Synth was first
// (2026-09-10), Keys second; the third one should not have to reinvent "rnd is
// a sample-and-hold on the phase wrap" a third time.
//
// No clock.h dependency ON PURPOSE: util cannot require the machine component,
// so the caller passes the core clock's tempo/pulses in. It also keeps this
// testable off-target.

enum { LFO_SINE = 0, LFO_TRI, LFO_SAW, LFO_SQR, LFO_RND, LFO_SHAPE_N };

// clock divisions as BEATS PER CYCLE, slowest first. One beat = a quarter, so
// 16 beats = 4 bars. Used only when sync is on and the core clock locks.
#define LFO_DIV_N 7
extern const float lfo_beats[LFO_DIV_N];
const char *lfo_div_name(int d);     // "4 bar" ... "1/16"
const char *lfo_shape_name(int s);   // "sine" ... "rnd"

typedef struct {
    float    phase;   // 0..1
    float    rnd;     // sample-and-hold value, for the RND shape
    uint32_t cyc;     // synced: which clock cycle the phase belongs to
} lfo_t;

// Advance by one audio block and return the LFO value, -1..1.
//   rate_hz  free-running rate, used when sync is off OR no clock is locked
//   sync/div rate comes from lfo_beats[div] against bpm instead
//   bpm      clock_core_beat_bpm() — <= 0 means no clock
//   pulses   clock_core_pulses(), ppb clock_core_ppb() (phase re-zero per cycle)
//   blockdur seconds of audio in this block
// No clock does NOT freeze the LFO: it falls back to the free rate.
float lfo_tick(lfo_t *l, int shape, float rate_hz, bool sync, int div,
               float bpm, uint32_t pulses, float ppb, float blockdur);
