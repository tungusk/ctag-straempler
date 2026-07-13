#pragma once
#include <stdint.h>
#include <stdbool.h>

// Drum sampler — up to 8 one-shot pads, each a mono PSRAM buffer, triggered
// from the CV inputs. Two trigger modes:
//   Direct    — CV1..CVn each fire their own pad via a Schmitt edge detector
//               (fire >= 75 % after having re-armed below 40 %; a channel that
//               never drops below the arm level — e.g. a jack pinned high —
//               simply never fires).
//   CV-select — the two clean gates TRIG1/TRIG2 are the triggers; each fires
//               the pad its selector CV points at (CV quantized to 0..n-1),
//               so a CV + gate addresses/sequences the pads.
// Engine (audio task) owns pos/playing/armed; the UI writes params.

#define DR_RATE       44100
#define DR_MAX_FRAMES (DR_RATE * 2)   // 2 s mono per pad (~176 KB PSRAM each)
#define DR_PADS       8
#define DR_WF_W       48              // waveform thumbnail columns (pad cell)
// Schmitt thresholds are RELATIVE to a per-source floor tracker: the CV
// channels idle at wildly different levels (1V/oct ~21 %, bipolar ~50 %, a
// broken jack pinned high), so absolute thresholds leave some inputs unable
// to re-arm. The tracker follows dips instantly and drifts up slowly.
// Sensitivity is a setting because some knob channels (5/8 on this unit)
// attenuate patched CV to ~half, shrinking the usable swing.
// 0=Low, 1=Med, 2=High → fire deltas {1500, 1100, 700}, arm {600, 450, 300}

typedef struct {
    int16_t *buf;                 // PSRAM mono buffer, DR_MAX_FRAMES
    volatile uint32_t len;        // frames loaded (0 = empty; zeroed during load)
    volatile uint32_t pos;        // play cursor (frames)
    volatile bool playing;
    volatile bool enabled;
    // level is UNITY at 255 and goes up to DR_LEVEL_MAX: past unity the pad is
    // driven into the soft clipper (knob6 at 12 o'clock = 100 %, clockwise from
    // there = overdrive). Old presets stored 0..255, which still means 0..100 %.
    volatile uint16_t level;
    volatile uint8_t pan;         // 0..255, 128 = centre (linear pan)
    volatile uint16_t decay_ms;   // linear decay envelope; 0 = play full sample
    // knob7 is neutral at noon: counter-clockwise chokes the decay, clockwise
    // drives ONE of these two, per pad (DR_CW_ATTACK / DR_CW_START)
    volatile uint8_t cw_mode;
    volatile uint16_t attack_ms;  // fade-in over the hit; 0 = the plain declick
    volatile uint8_t start_off;   // head skipped, /255 of the sample; 0 = the top
    volatile uint16_t loop_ms;    // stutter loop length; 0 = play straight through
    volatile uint8_t loop_reps;   // cap the stutter at N repeats; 0 = run out the
                                  // sample's length, DR_REPS_INF = never stop.
                                  // THIS is the retrig control: "4 reps of a 60 ms
                                  // loop" is a fill, INF is a held drone
    volatile int trig_src;        // Direct mode: CV input (0..7) that fires this
                                  // pad, default = pad index; routable so pads
                                  // can dodge bad jacks (e.g. this unit's ch4)
    volatile uint8_t vel;         // velocity of the current hit, 0..255
    volatile bool hit;            // set on trigger, cleared by the UI (pad flash)
    bool armed;                   // Schmitt state, engine only
    int  base;                    // tracked floor of the routed CV source
    // declick (engine only): samples rarely start/end at zero crossings, so a
    // short attack ramp + tail ramp frame the one-shot, and a retrigger fades
    // the playing voice out (~0.7 ms) before restarting instead of jumping
    bool retrig;                  // fade-out-then-restart in progress
    int  fade;                    // retrig fade level, 256..0
    uint8_t vel_next;             // velocity for the queued restart
    char sample[24];              // loaded library sample id ("" = empty)
    // waveform thumbnail for the pad cell, built from the RAM buffer at load
    // (no SD pass needed — the whole sample is already in PSRAM). Peak per
    // column, 0..255, same scale the sampler uses.
    uint8_t wf[DR_WF_W];
    bool    wf_valid;
} dr_pad_t;

// CV6/CV7 perform the SELECTED pad: knob6 = level, knob7 = decay (knobs 6/7 are
// this unit's two fully-good CV channels). They take over a value only when the
// control MOVES — otherwise selecting a pad, or booting, would slam its stored
// level/decay to wherever the knob happens to be sitting.
#define DR_MOD_LEVEL_CV 5         // 0-based: CV6
#define DR_MOD_DECAY_CV 6         // 0-based: CV7
#define DR_MOD_MOVE     100       // ~2.5 % of range: past CV noise, under a nudge

#define DR_LEVEL_UNITY  255       // knob6 at 12 o'clock: the sample, unchanged
#define DR_LEVEL_MAX    1023      // fully clockwise: 4x into the soft clipper

// what knob7 CLOCKWISE of noon drives, chosen per pad on the Pads page
#define DR_CW_LOOP      0         // stutter: loop the head, shorter the further CW
#define DR_CW_ATTACK    1         // fade the hit in, up to DR_ATTACK_MAX
#define DR_CW_START     2         // skip into the sample, up to DR_START_MAX/255
#define DR_CW_MODES     3
#define DR_ATTACK_MAX   400       // ms at full clockwise
#define DR_START_MAX    192       // /255 of the sample: 75 % in, still leaves a tail
// the loop: just past noon it's a long roll, hard right it's a buzz. The pad
// still plays for as long as the sample WOULD have — the loop repeats inside
// that window rather than droning on, so a one-shot stays a one-shot.
#define DR_LOOP_MAX_MS  400
#define DR_LOOP_MIN_MS  8
#define DR_REPS_INF     255       // loop until the pad is retriggered — a drone

typedef struct {
    dr_pad_t pad[DR_PADS];
    volatile int n_pads;          // 4 or 8
    volatile int sel_pad;         // pad the encoder/CV6/CV7 are aimed at
    volatile bool cv_select;      // false = Direct, true = TRIG1/2 + selector CV
    volatile bool velocity;       // Direct mode: scale hit by CV level at fire time
    volatile bool cv_mod;         // CV6/CV7 perform the selected pad
    volatile int sens;            // trigger sensitivity 0..2 (see thresholds above)
    volatile int sel_src[2];      // selector CV channel per trig (CV-select mode)
    uint8_t prev_trig;            // gate edge state (seed 0x03 = idle high)
} dr_state_t;

extern dr_state_t dr;

// UI-side helpers (drum.c); do SD I/O — call from the UI task only
int  drum_load_pad(int pad, const char *name);
void drum_clear_pad(int pad);
