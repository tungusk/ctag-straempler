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
    volatile uint8_t level;       // 0..255
    volatile uint8_t pan;         // 0..255, 128 = centre (linear pan)
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
} dr_pad_t;

typedef struct {
    dr_pad_t pad[DR_PADS];
    volatile int n_pads;          // 4 or 8
    volatile bool cv_select;      // false = Direct, true = TRIG1/2 + selector CV
    volatile bool velocity;       // Direct mode: scale hit by CV level at fire time
    volatile int sens;            // trigger sensitivity 0..2 (see thresholds above)
    volatile int sel_src[2];      // selector CV channel per trig (CV-select mode)
    uint8_t prev_trig;            // gate edge state (seed 0x03 = idle high)
} dr_state_t;

extern dr_state_t dr;

// UI-side helpers (drum.c); do SD I/O — call from the UI task only
int  drum_load_pad(int pad, const char *name);
void drum_clear_pad(int pad);
