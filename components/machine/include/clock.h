#pragma once
#include <stdint.h>
#include <stdbool.h>

// Shared CV clock detector — rising-edge on a thresholded signal, period
// averaged over an 8-interval ring, 20..300 BPM sanity gate, lock after two
// edges, unlock when the clock stops. Used by the looper and glitch machines.
// Each machine keeps its own beatclock_t and calls clock_tick() once per frame.

typedef struct {
    bool     prev_high;
    uint32_t since;          // samples since last rising edge
    uint32_t ring[8];
    int      ring_n;
    uint32_t period;         // median samples per pulse (0 = none)
    uint32_t idle;           // samples since last edge (lock timeout)
    float    bpm;            // pulse rate as BPM (divide by PPB yourself)
    bool     locked;
    // pulse-interval sanity gate. clock_reset() sets 20..300 BPM (the
    // looper/glitch 1-pulse-per-beat assumption); a machine expecting
    // faster/slower pulses (e.g. the deck at 4 PPQN) MUST widen these or
    // every legitimate interval is rejected and the BPM readout is built
    // from missed-edge garbage.
    uint32_t period_min;     // samples
    uint32_t period_max;
    uint8_t  split_run;      // consecutive ~2x intervals split as missed edges
    uint8_t  ghost_run;      // consecutive raw edges at ~half period (faster-clock escape)
    uint32_t since_raw;      // frames since the previous RAW edge (accept or not)
} beatclock_t;

void clock_reset(beatclock_t *c);

// advance one frame with the clock line level (0..4095, e.g. a CV channel or
// a synthesised 0/4095 from a trig). Returns true on a detected quarter edge.
bool clock_tick(beatclock_t *c, uint16_t cv);
