#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "clock.h"
#include "svf.h"

// Dual-deck — a clock-locked track BLENDER, not a DJ rig (the design reframe:
// manual beatmatching is what eats controls; here both decks phase-lock to the
// SAME conditioned clock, so they are beatmatched by construction and the
// performer's verbs shrink to fit the panel):
//
//   TR1 tap  = deck A quantized start/restart (next BAR)   TR1 hold = stop
//   TR2 tap/hold = deck B, same
//   knob6/CV6 = equal-power crossfade A<->B (takeover fade auto-moves it on a
//               deck start; moving the knob past a deadband grabs it back)
//   knob7/CV7 = master DJ filter on the summed mix (the deck's LP/HP sweep)
//   encoder   = focus (turn) / track browser for the focused deck (press) /
//               Setup (long)
//
// Each deck is a dk-style streaming voice: PSRAM ring, ONE shared reader task
// owning all SD I/O, per-deck PLL against the shared clockin_t. A stopped deck
// PARKS AT ITS CUE (grid downbeat) with the ring pre-filled, so a quantized
// start is a flag flip — zero SD wait on the bar boundary.
// Tempo truth comes from the track's sidecar stamp (deck-analyzed or sampler3
// clock-stamped takes); there is NO analysis engine in this machine. An
// unstamped track shows "no grid" and free-runs at unity.

#define DD_RATE        44100
#define DD_RING_FRAMES (DD_RATE * 6)          // 6 s stereo PSRAM ring x2 (~2.1 MB)
#define DD_LOW_WATER   (DD_RATE / 3)
#define DD_NAME_LEN    24
#define DD_WF_W        120                    // per-panel waveform columns

typedef struct {
    // streaming (reader owns wpos + all seeks; engine advances rpos while playing)
    int16_t *ring;                 // PSRAM, DD_RING_FRAMES * 2
    volatile uint32_t wpos;
    volatile uint32_t rpos_i;
    double   rpos_f;
    volatile uint32_t file_frames;
    volatile bool playing;
    volatile bool loading;         // reader (re)filling after seek/track change
    volatile uint32_t seek_to;     // seek protocol: requesters set flags only,
    volatile bool seek_req;        // the READER applies them (deck lesson)
    volatile bool track_req;
    char pending[DD_NAME_LEN];
    char track[DD_NAME_LEN];       // loaded track id ("" = none)

    // grid + sync (sidecar stamp; no analysis in this machine)
    volatile float track_bpm;      // 0 = unstamped -> free-run "no grid"
    volatile uint32_t grid_offset;

    // quantized transport: armed ops fire on the next bar boundary
    volatile bool arm_start;       // start/restart from the cue
    volatile bool arm_stop;        // stop + re-park at the cue

    // PLL (deck math, per deck, against the shared clock)
    float phase_int;
    volatile float phase_err;      // UI display
    float rate_sm;
    volatile float rate;           // UI display
    float out_gain;                // declick ramp

    // waveform thumbnail (reader builds one column per idle pass)
    uint8_t wf[DD_WF_W];
    volatile int wf_state;         // 0 none, 1 building, 2 valid
    int wf_col;

    volatile uint32_t dbg_starve;
} dd_deck_t;

typedef struct {
    dd_deck_t d[2];
    volatile int focus;            // encoder lane focus (0 = A, 1 = B)

    // shared conditioned clock (the whole point of this machine)
    clockin_t ci;
    volatile int clk_src;          // CV channel (default CV8)
    volatile int ppb_idx;          // into dd_ppb[] (mult/div)
    volatile uint32_t pulses;      // accepted pulses since lock (bar phase ref)

    // crossfade: knob6 manual + takeover automation. xf 0 = full A, 1 = full B.
    volatile int xf_cv;            // raw knob (UI display)
    float xf;                      // effective position (smoothed)
    float auto_target;             // takeover fade destination
    volatile bool auto_active;     // an auto-fade is in flight
    float auto_step;               // per-block xf increment while auto-fading
    int   xf_ref;                  // manual-grab latch: knob position at auto
                                   // start; move past DD_XF_GRAB counts = grab
    volatile bool manual;          // knob is live (grabbed / no auto pending)
    volatile int fade_beats;       // Setup: 0 = cut, else 1/4/8 beats

    // master DJ filter (knob7 on the summed mix; deck sweep, fixed q)
    volatile int filt_cv;
    float flt_f;
    volatile int flt_mode;         // 0 off, 1 LP, 2 HP (UI)
    svf_t flt_l, flt_r;
} dd_state_t;

extern dd_state_t dd;
extern const float dd_ppb[6];
extern const char *const dd_ppb_names[6];

// UI-side (SD-touching; UI/background tasks only)
int  dualdeck_load_track(int deck, const char *name);
// audio-task-safe transport arms (engine fires them on the bar)
void dualdeck_arm_start(int deck);
void dualdeck_arm_stop(int deck);
