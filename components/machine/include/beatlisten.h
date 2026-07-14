#pragma once
#include <stdint.h>
#include <stdbool.h>

// beatlisten — the core "ear": a machine-independent service that derives a
// clock from the LINE-IN audio bus and publishes it as a synthesized 0/4095
// level, selectable by any machine as clock source CLK_SRC_AUDIO (clock.h).
// The level feeds each machine's existing clockin_t front-end unchanged, so
// clk.since/clk.period stay valid in sample units and the deck PLL works
// as-is. Fed from the audio task (beatlisten_push, beside the recording tap);
// tempo/phase estimation runs on an unpinned prio-4 task (deck_analysis
// precedent — the streaming port of its onset->flux->ACF pipeline).
//
// STEADINESS CONTRACT (Arlo: "none of this octave hopping, jumping around"):
// GROOVE never changes octave while locked (candidates fold to the current
// tempo first), slews <=1 BPM/s, retracks only after ~5 s of sustained
// confident disagreement, and freewheels at the last tempo through dropouts.

typedef enum {
    BL_OFF    = 0,
    BL_PULSE  = 1,   // hard pulses / click tracks: full-band onset gate
    BL_KICK   = 2,   // P2: low-band (kick) onsets — P1 falls back to PULSE
    BL_FLUX   = 3,   // P2: broadband transient onsets — P1 falls back to PULSE
    BL_GROOVE = 4,   // tempo model: steady predicted grid, the headline mode
    BL_NMODES = 5
} bl_mode_t;

typedef enum {
    BL_ST_OFF       = 0,
    BL_ST_ONSET     = 1,   // a pulse mode is active (no tempo model)
    BL_ST_SILENT    = 2,   // GROOVE: no signal, nothing locked yet
    BL_ST_LISTEN    = 3,   // GROOVE: acquiring (silent output until locked)
    BL_ST_LOCKED    = 4,   // GROOVE: grid running, tracking
    BL_ST_FREEWHEEL = 5    // GROOVE: signal gone, grid holds last tempo/phase
} bl_state_t;

typedef struct {
    int   mode;        // bl_mode_t
    int   state;       // bl_state_t
    float bpm;         // locked grid tempo (0 when no model)
    float conf;        // last ACF confidence 0..1 (GROOVE)
    int   cost_us;     // audio-task cost EMA (1450 us = 100% of the tick)
} bl_status_t;

void beatlisten_init(void);                        // before the audio task starts

// audio task, once per block, unconditional (OFF = one branch + return).
// No SD, no blocking, no allocation — same rules as machine process().
void beatlisten_push(const int32_t in[64]);

// the synthesized clock line (0 or 4095), consumed via clock_source_level()
uint16_t beatlisten_level(void);

// clock OUT over the audio jacks: when enabled, overwrites one output channel
// with 10 ms full-scale pulses on each beat (PO/Volca-style sync — a short
// pulse survives the AC-coupled output as a sharp spike). ch: 0=off 1=L 2=R.
// Called from the audio task after the machine's process().
void beatlisten_out_render(int32_t out[64]);
void beatlisten_set_out(int ch);
int  beatlisten_get_out(void);

void beatlisten_set_mode(int mode);                // implies relock; allocs on first enable
int  beatlisten_get_mode(void);
void beatlisten_relock(void);                      // drop the model, re-acquire
void beatlisten_get_status(bl_status_t *out);
