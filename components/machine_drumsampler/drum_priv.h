#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "svf.h"
#include "reverb.h"

// Drum sampler — four one-shot pads, each a mono PSRAM buffer, triggered from the
// CV inputs. (A pad can carry a second, choking layer, which is what the old
// 8-pad mode was really for — see dr_pad_t.layered.) Two trigger modes:
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
#define DR_PADS       4      // four pads, full stop: a layered pad already
                                    // carries two sounds, which is what 8 was for
#define DR_WF_W       48              // waveform thumbnail columns (pad cell)
// Schmitt thresholds are RELATIVE to a per-source floor tracker: the CV
// channels idle at wildly different levels (1V/oct ~21 %, bipolar ~50 %, a
// broken jack pinned high), so absolute thresholds leave some inputs unable
// to re-arm. The tracker follows dips instantly and drifts up slowly.
// Sensitivity is a setting because some knob channels (5/8 on this unit)
// attenuate patched CV to ~half, shrinking the usable swing.
// 0=Low, 1=Med, 2=High → fire deltas {1500, 1100, 700}, arm {600, 450, 300}

// A pad holds TWO layers that share ONE voice: triggering either interrupts the
// other. That is the open/closed hi-hat, and it is the intended replacement for
// 8-pad mode — 4 cells carrying 8 sounds and 8 triggers. Optional (dr.layers_on);
// with it off the machine behaves exactly as it did before layers existed.
//
// Per-layer: the buffer, its trigger CV, and its OWN Schmitt state — the two
// layers watch different CV channels with different idle floors, so a shared
// detector would cross-arm. Everything performable (level/pan/decay/cw/retrig) is
// per PAD, not per layer: knob6/knob7 perform the pad, and a knob that meant a
// different thing depending on which layer last fired would be unplayable. A pad
// is a channel strip with two sounds in it.
#define DR_LAYERS    2
#define DR_SRC_NONE  (-1)

// the retrigger fade. Same-layer keeps the original ~0.7 ms (a hit restarting
// itself); a CROSS-layer choke is slower — 0.7 ms reads as a gate click, ~3 ms
// reads as one sound cutting another off.
#define DR_RETRIG_STEP 8
#define DR_CHOKE_STEP  2

typedef struct {
    int16_t *buf;                 // PSRAM mono, DR_MAX_FRAMES; NULL = not allocated
    volatile uint32_t len;        // frames loaded (0 = empty; zeroed during load)
    volatile int trig_src;        // Direct mode: CV that fires THIS layer;
                                  // DR_SRC_NONE = nothing does
    bool armed;                   // Schmitt state, engine only — PER LAYER
    int  base;                    // tracked floor of this layer's CV source
    char sample[24];              // loaded library sample id ("" = empty)
    // waveform thumbnail for the pad cell, built from the RAM buffer at load
    // (no SD pass needed — the whole sample is already in PSRAM). Peak per
    // column, 0..255, same scale the sampler uses.
    uint8_t wf[DR_WF_W];
    bool    wf_valid;
} dr_layer_t;

typedef struct {
    dr_layer_t ly[DR_LAYERS];
    volatile bool layered;        // THIS pad has a B layer that chokes its A. Per
                                  // pad, not global (Arlo): a kit is usually one
                                  // hi-hat pair and three plain one-shots.
    // ---- ONE voice, shared by both layers: this is what makes the choke ----
    volatile uint32_t pos;        // play cursor (frames)
    volatile bool playing;
    volatile uint8_t cur;         // the layer the voice is currently READING
    volatile bool enabled;
    // level is UNITY at 255 and goes up to DR_LEVEL_MAX: past unity the pad is
    // driven into the soft clipper (knob6 at 12 o'clock = 100 %, clockwise from
    // there = overdrive). Old presets stored 0..255, which still means 0..100 %.
    volatile uint16_t level;
    volatile uint8_t pan;         // 0..255, 128 = centre (linear pan)
    volatile uint8_t rv_send;     // 0..255 REVERB SEND (0 = dry, the default:
                                  // a kick belongs out of the tank)
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
    volatile uint8_t vel;         // velocity of the current hit, 0..255
    volatile bool hit;            // set on trigger, cleared by the UI (pad flash)
    volatile uint8_t hit_layer;   // which layer fired it (the dot's shape)
    // declick (engine only): samples rarely start/end at zero crossings, so a
    // short attack ramp + tail ramp frame the one-shot, and a retrigger fades
    // the playing voice out before restarting instead of jumping. With layers,
    // "retrig" means: fade whatever is sounding, THEN start next_layer — which
    // may be the other buffer entirely.
    bool retrig;                  // fade-out-then-(re)start in progress
    int  fade;                    // fade level, 256..0
    int  fade_step;               // DR_RETRIG_STEP or DR_CHOKE_STEP
    uint8_t vel_next;             // velocity for the queued (re)start
    uint8_t next_layer;           // layer for the queued (re)start
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
#define DR_CW_NONE      3         // clockwise does NOTHING: knob7 is a decay-only
                                  // control, and half its travel is a safe zone
#define DR_CW_MODES     4
#define DR_ATTACK_MAX   400       // ms at full clockwise
#define DR_START_MAX    192       // /255 of the sample: 75 % in, still leaves a tail
// the loop: just past noon it's a long roll, hard right it's a buzz. The pad
// still plays for as long as the sample WOULD have — the loop repeats inside
// that window rather than droning on, so a one-shot stays a one-shot.
#define DR_LOOP_MAX_MS  400
#define DR_LOOP_MIN_MS  8
#define DR_REPS_INF     255       // loop until the pad is retriggered — a drone

// MASTER FILTER — a fifth thing the encoder can select in Live (a box in the
// menu bar, not a grid cell). knob6 = the deck's DJ sweep (centre bypass, left
// = LP down, right = HP up), knob7 = resonance, which the deck doesn't have.
// Runs on the mixed output of every pad.
#define DR_FLT_FMAX   1.0f        // Chamberlin stability ceiling. LOWER than the
                                  // deck's 1.2: the deck can push that hard only
                                  // because its damping is nailed at 0.9 — with a
                                  // resonance knob, 1.2 self-oscillates.
#define DR_Q_CLEAN    2.0f        // damping: HIGHER is cleaner (q is 1/resonance)
#define DR_Q_SQUELCH  0.10f

typedef struct {
    dr_pad_t pad[DR_PADS];
    volatile int sel_pad;         // pad the encoder/CV6/CV7 are aimed at
    volatile bool sel_filter;     // ...unless the encoder is on the FILTER box.
                                  // A separate flag, NOT a sel_pad sentinel:
                                  // dr.pad[dr.sel_pad] is indexed all over, and
                                  // sel_pad == n_pads would index pad[8].
    volatile bool cv_select;      // false = Direct, true = TRIG1/2 + selector CV
    volatile bool velocity;       // Direct mode: scale hit by CV level at fire time
    volatile bool cv_mod;         // CV6/CV7 perform the selected pad
    volatile int sens;            // trigger sensitivity 0..2 (see thresholds above)
    volatile int sel_src[2];      // selector CV channel per trig (CV-select mode)
    uint8_t prev_trig;            // gate edge state (seed 0x03 = idle high)

    // --- master filter ---
    volatile bool flt_box;        // Setup: does the box exist at all
    volatile bool flt_on;         // encoder click on the box
    volatile int  flt_cv;         // last ACCEPTED sweep knob (knob6), UI reads
    volatile int  flt_res_cv;     // last ACCEPTED resonance knob (knob7), UI reads
    volatile int  flt_mode;       // 0 bypass / 1 LP / 2 HP  (engine writes, UI reads)
    // knob pickup: selecting the box must NOT slam the filter to wherever knob6
    // happens to sit. The UI arms these at -1; the engine seizes the reference on
    // its next block and applies nothing until the knob moves past DR_MOD_MOVE —
    // after which it tracks CONTINUOUSLY (a DJ sweep has to be smooth, unlike the
    // pads' stepped take-over).
    volatile int  flt_ref_f, flt_ref_q;
    bool  flt_take_f, flt_take_q;
    float flt_f, flt_q;           // slewed coefficient + damping (engine only)
    svf_t flt_l, flt_r;           // engine only

    // master REVERB (shared util/reverb.h — the FX rack's second brick after
    // the filter): sits AFTER the filter on the summed mix. Slab is LAZY
    // (an OFF reverb costs no PSRAM); menu/preset own mode+mix, engine only
    // calls reverb_block_i32 (self-gating on mode/slab).
    reverb_t rv;
    // the reverb is a SEND bus, not an insert (Arlo, ear test: "we don't want
    // to send a big kick drum through the reverb"). Each pad decides how much
    // of itself goes in (rv_send 0..255); the master Rev Mix row is the RETURN
    // level. Dry always passes at full level.

    // knob take-over state for the PADS (was function-static in drum_process,
    // where it survived stop()/start() and let a machine re-entry inherit the
    // last session's reference)
    int  knob_last[2];
    bool knob_seen;
} dr_state_t;

extern dr_state_t dr;

// UI-side helpers (drum.c); do SD I/O — call from the UI task only
int  drum_load_layer(int pad, int ly, const char *name);
void drum_clear_layer(int pad, int ly);
int  drum_load_pad(int pad, const char *name);   // = layer A
void drum_clear_pad(int pad);
