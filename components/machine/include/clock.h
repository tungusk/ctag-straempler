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

// ---- conditioned clock INPUT (the shared front-end) --------------------------
// Everything each machine was copy-pasting around beatclock_t: a floor-tracked
// Schmitt over any CV channel (fixed thresholds misfire on attenuated/offset
// channels), a synthesized square into the detector, an AC-coupling ghost-edge
// gate on the raw sync edges (pulse tails ring and refire the Schmitt —
// measured +83.6ms ghost-quantized takes), and the clock's pulses-per-beat
// carried WITH the detector so tempo math stops hardcoding "4".
typedef struct {
    beatclock_t clk;
    int      base;           // tracked channel floor
    bool     high;           // Schmitt state
    float    ppb;            // pulses per beat (1/2/4/8; deck also sub-beat)
    uint32_t edge_since;     // ghost gate: frames since last ACCEPTED edge
    // raw Schmitt-fire diagnostics (pre-detector, pre-ghost-gate) — /status
    // surfaces these to tell jack/pulse-width trouble (ghost + missed edges
    // at the input) apart from detector trouble
    uint32_t raw_fires;      // rising fires since reset
    uint32_t raw_iv;         // frames between the last two fires
    uint32_t raw_since;      // frames since the last fire
} clockin_t;

void clockin_reset(clockin_t *ci, float ppb);
void clockin_set_ppb(clockin_t *ci, float ppb);   // rescales the sanity gates
// run one audio block: condition `cv`, tick the detector `frames` times.
// Returns true when a ghost-gated rising edge fired within this block.
bool clockin_block(clockin_t *ci, uint16_t cv, int frames);
// tempo of the BEAT (pulse rate / ppb); 0 when unlocked
static inline float clockin_beat_bpm(const clockin_t *ci)
{
    return (ci->clk.locked && ci->clk.bpm > 0 && ci->ppb > 0)
           ? ci->clk.bpm / ci->ppb : 0;
}
