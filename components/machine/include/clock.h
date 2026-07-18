#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "machine.h"   // machine_io_t for clock_source_level()

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
    // OCTAVE PREFERENCE (Arlo: "default to a safe range (80-140) on its
    // initial detection"). Tempo is octave-ambiguous: the same pulse train
    // reads as 70 or 140 depending on what you call a beat. Rather than touch
    // the detector (its period feeds the phase reference), we fold the
    // INTERPRETATION: oct multiplies ppb so the implied BEAT lands in the
    // musical band. Hysteretic (76..148) so it can't flap at the edges;
    // a machine's manual clock-scale still overrides on top.
    float    oct;            // 0.25 .. 4 (1 = as detected)
} clockin_t;

#define CLOCKIN_BPM_LO  80.0f
#define CLOCKIN_BPM_HI  140.0f

// pulses per beat AFTER the octave fold — what tempo math should use
static inline float clockin_ppb_eff(const clockin_t *ci)
{
    float o = (ci->oct > 0) ? ci->oct : 1.0f;
    return ci->ppb * o;
}

// ---- shared clock-source selection -------------------------------------------
// One encoding for every machine's clk_src (generalized from the looper, whose
// 8/9 TR values this adopts): 0..7 = CV1..CV8, 8/9 = TR1/TR2 (active low),
// 10 = AUDIO (the beatlisten service's synthesized level). Machines feed
// clock_source_level() straight into their clockin_block().
#define CLK_SRC_TR1   8
#define CLK_SRC_TR2   9
#define CLK_SRC_AUDIO 10
#define CLK_SRC_INT   11   // internal clock (machine's manual BPM, no external in)
#define CLK_SRC_OFF   12   // no clock — free / un-clocked (no tempo grid)
#define CLK_SRC_COUNT 13

uint16_t clock_source_level(int src, const machine_io_t *io);
const char *clock_source_name(int src);   // "CV1".."CV8","TR1","TR2","AUDIO"

// Machines whose trig inputs already have jobs (deck transport, sampler3
// gates, glitch stutter) must NOT offer TR1/TR2 as clock sources — a clock
// patched into a trig would drive both the detector AND that trig's grammar
// (the looper is the exception: it masks its clock trig out of button
// handling). These helpers cycle/clamp over CV1..CV8 + AUDIO only.
int clock_source_cycle_cv_audio(int src, int dir);   // menu FWD/BWD
int clock_source_clamp_cv_audio(int src);            // preset load (dflt CV8)

void clockin_reset(clockin_t *ci, float ppb);
void clockin_set_ppb(clockin_t *ci, float ppb);   // rescales the sanity gates
// run one audio block: condition `cv`, tick the detector `frames` times.
// Returns true when a ghost-gated rising edge fired within this block.
bool clockin_block(clockin_t *ci, uint16_t cv, int frames);
// tempo of the BEAT (pulse rate / ppb); 0 when unlocked
static inline float clockin_beat_bpm(const clockin_t *ci)
{
    float p = clockin_ppb_eff(ci);
    return (ci->clk.locked && ci->clk.bpm > 0 && p > 0) ? ci->clk.bpm / p : 0;
}
