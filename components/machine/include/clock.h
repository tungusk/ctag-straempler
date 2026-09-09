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
// True when CV channel `ch` (0-based: 0 = CV1) is being used as the CLOCK SOURCE.
//
// CV5-CV8 are knob+jack channels on this hardware, so a clock patched into one
// is read TWICE: by clockin_block() and by whatever parameter the machine maps
// to that knob. Tape shipped with clk_src defaulting to CV8 *and* CV8 mapped to
// Drive, so clocking the module the documented way slammed the drive stage with
// the pulse train — audible rhythmic distortion with every FX slot off, plus a
// machine_state_dirty() per pulse churning AUTOSAVE.JSN to the card (2026-07-25).
//
// Tape was the only DEFAULT collision; Deck/DoubleDecker/Glitch/Looper/Tracker
// map CV6/CV7 while defaulting the clock to CV8, so theirs is latent — pick CV6
// or CV7 as the clock source and the same thing happens. Callers skip the
// parameter write when this returns true.
static inline bool clock_src_is_cv(int src, int ch)
{
    return src >= 0 && src <= 7 && src == ch;
}

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

// ---- the CORE clock (2026-09-08) -----------------------------------------------
// ONE clock interpreter for the whole module, owned by the core and ticked by the
// audio task before the active machine's process() (right after beatlisten, whose
// synthesized level it can consume). Machines READ it (clock_core()) and must not
// run a detector of their own: the lock survives machine switches, "the module's
// tempo" exists in exactly one place, and the source / pulses-per-beat / internal
// BPM are GLOBAL settings (CONFIG.JSN clk_src / clk_ppq / clk_bpm / clk_auto,
// persisted by whoever calls the setters — ui.c at boot, /settings, machine Setup
// rows write through). Sources: CV1-8 / TR1-2 / AUDIO as before; INT = the core's
// own pulse generator at int_bpm x ppb fed through the SAME detector, so
// locked/period/since behave identically and every machine's phase math works
// unchanged; OFF = no clock (never locks). AUTO fallback (clk_auto): with an
// external source selected and no lock for ~2 s, the INT pulses take over until
// the external source shows an edge again (external always wins).
const clockin_t *clock_core(void);                    // read-only view for machines
void  clock_core_block(const machine_io_t *io, int frames);   // AUDIO TASK ONLY
bool  clock_core_edge(void);        // accepted (ghost-gated) edge in the last block
uint32_t clock_core_pulses(void);   // accepted-edge counter (bar math, pre/post compares)
int   clock_core_src(void);         void clock_core_set_src(int src);     // CLK_SRC_*
float clock_core_ppb(void);         void clock_core_set_ppb(float ppb);   // 1/2/4/8
float clock_core_int_bpm(void);     void clock_core_set_int_bpm(float bpm);
bool  clock_core_auto(void);        void clock_core_set_auto(bool on);    // INT fallback
bool  clock_core_fallback(void);    // INT pulses currently standing in for the source
float clock_core_beat_bpm(void);    // locked ? detected BEAT tempo : 0 (INT locks too)
