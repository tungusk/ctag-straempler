#pragma once
#include <stdint.h>
#include <stdbool.h>

// Shared trig-gate press grammar (the unified TR contract, 2026-07-13):
// tap and hold on one active-low gate input, resolved into events. Replaces
// the hand-rolled prev/hold/fired triples that had grown in tracker, deck and
// dualdeck (each subtly different).
//
//   TG_PRESS     — falling edge, the instant the gate goes down
//   TG_HOLD      — fires ONCE when the hold crosses the 0.6 s threshold
//                  (for at-threshold actions like dualdeck's quantized stop)
//   TG_REL_SHORT — released before the threshold (a tap)
//   TG_REL_LONG  — released after the threshold (hold-release actions:
//                  restart-on-release, momentary-loop exit)
//
// Call once per audio block with the gate's level bit. Header-only.

typedef enum {
    TG_NONE = 0,
    TG_PRESS,
    TG_HOLD,
    TG_REL_SHORT,
    TG_REL_LONG,
} tg_event_t;

typedef struct {
    uint32_t held;        // frames the gate has been down (0 = up)
    bool     long_fired;  // TG_HOLD already emitted this press
    uint8_t  dn;          // consecutive blocks seen DOWN (debounce)
    uint8_t  up;          // consecutive blocks seen UP
    bool     state;       // the DEBOUNCED level
    bool     pulse;       // a SHORT validated gate is being synthesised as a tap
} trig_gate_t;

#define TG_LONG_FRAMES ((uint32_t)(0.6f * 44100.0f))

// DEBOUNCE (2026-07-13, bench-caught). The gate inputs are active-low and a
// FLOATING input glitches: with NOTHING patched into TR2, /status caught a stray
// low sample — and since TR2 press = LOOP TOGGLE fires on the instant the gate
// goes down, one noisy block flipped the loop underneath Arlo ("it falls out of
// loop mode and the loops are jumping around on their own"). A press must now
// PERSIST for TG_DEBOUNCE blocks before it counts, and a release likewise.
//
// TWO SOURCES, EACH FOR WHAT IT IS GOOD AT (resolved 2026-07-14):
//   - `raw` = the per-block LEVEL sample. Debounced by TG_DEBOUNCE blocks, which
//     rejects a floating-input glitch that happens to coincide with the sample.
//   - `rising` = a VALIDATED assert edge from the acquisition layer (audio.c GPIO
//     ISRs; the low must hold >= 200 us). This catches a short gate that fell
//     ENTIRELY BETWEEN block samples — which the level path structurally cannot see,
//     because a block is 0.726 ms and a eurorack trigger can be 1 ms.
//
// Together they kill both bugs at once: a 1 ms gate always registers (via `rising`,
// synthesised as a tap), and a sub-200 us glitch never does (it is neither validated
// nor able to survive the level debounce). This is why TG_DEBOUNCE STAYS AT 2 rather
// than going back to 1 as the acquisition note suggested — with the rising path in
// place, the debounce no longer costs us short gates, and it still guards the level
// path against a floating pin.
#define TG_DEBOUNCE 2

// `down` = gate active (caller maps the active-low trig bit); `frames` = block
// size. Returns at most one event per call — PRESS on the down edge, HOLD at
// the threshold, REL_* on the up edge.
// `raw` = the pin this block (active = asserted). `rising` = a validated assert edge
// since the last block (io->trig_rising). Returns at most one event per call.
static inline tg_event_t trig_gate_step_ex(trig_gate_t *g, bool raw, bool rising, int frames)
{
    // debounce the LEVEL: `raw` is the pin, `g->state` is what the grammar sees
    if (raw) {
        g->up = 0;
        if (g->dn < 255) g->dn++;
        if (g->dn >= TG_DEBOUNCE) g->state = true;
    } else {
        g->dn = 0;
        if (g->up < 255) g->up++;
        if (g->up >= TG_DEBOUNCE) g->state = false;
    }
    bool down = g->state;

    // A SHORT validated gate is synthesised as a TAP: press now, release next block.
    // Without this a 1 ms trigger is simply invisible — it can fall entirely between
    // two level samples.
    if (g->pulse) {
        if (down) g->pulse = false;              // the level path owns it now
        else if (!raw) {                         // the pulse is over: complete the tap
            g->pulse = false;
            g->held = 0;
            g->long_fired = false;
            return TG_REL_SHORT;
        }
    } else if (!down && rising && g->held == 0) {
        g->pulse = true;                         // press now; the release follows
        g->held = (uint32_t)frames;
        g->long_fired = false;
        return TG_PRESS;
    }

    if (down) {
        if (g->held == 0) {
            g->held = (uint32_t)frames;
            g->long_fired = false;
            return TG_PRESS;
        }
        g->held += (uint32_t)frames;
        if (!g->long_fired && g->held >= TG_LONG_FRAMES) {
            g->long_fired = true;
            return TG_HOLD;
        }
        return TG_NONE;
    }
    if (g->held > 0) {
        bool was_long = g->long_fired || g->held >= TG_LONG_FRAMES;
        g->held = 0;
        g->long_fired = false;
        g->pulse = false;
        return was_long ? TG_REL_LONG : TG_REL_SHORT;
    }
    return TG_NONE;
}

// level-only shim: a machine that does not care about sub-block gates
static inline tg_event_t trig_gate_step(trig_gate_t *g, bool raw, int frames)
{
    return trig_gate_step_ex(g, raw, false, frames);
}

// ---- BOTH-GATE COMBO: the shared RESYNC gesture (Arlo, 2026-07-13) ----------
// Hold TR1 and TR2 together, release, and the machine's beat lands ON the
// release — a manual re-anchor of the PLL that needs no knob. It is a general
// contract, not a deck feature: any clock-following machine can adopt it.
//
//   TC_ARMED — both gates have been down past TC_HOLD_FRAMES (show it in the UI)
//   TC_FIRE  — the first gate came back up: THIS instant is the beat
//
// (Timing note: process() runs once per 32 stereo frames = 0.726 ms, since
// MACHINE_BLOCK is 64 INTERLEAVED samples. Earlier comments here said "~3 ms" for a
// 2-block debounce; the real figure is ~1.45 ms. Frame-counted thresholds like
// TG_LONG_FRAMES are unaffected — only BLOCK-counted ones were mis-stated.)
//
// The combo ARMS at 0.35 s, comfortably below the individual TG_LONG_FRAMES
// (0.6 s), so it always beats TR1-hold / TR2-hold to the punch — and short
// simultaneous gates (a sequencer hitting both at once) pass through untouched
// because nothing is suppressed until the combo actually arms.
//
// Usage: step the two per-gate trig_gate_t's as usual, step this too, and act
// on the individual events ONLY while !trig_combo_busy() — that swallows the
// HOLD and REL_LONG the two gates will otherwise emit during the gesture.

typedef enum { TC_NONE = 0, TC_ARMED, TC_FIRE } tc_event_t;

typedef struct {
    uint32_t both;      // frames both gates have been down together
    bool armed;         // crossed the arm threshold: the gesture owns the trigs
    bool latched;       // fired; swallow gate events until BOTH come back up
} trig_combo_t;

#define TC_HOLD_FRAMES ((uint32_t)(0.35f * 44100.0f))

// true while the gesture owns the trigs — the machine must ignore its own
// per-gate events for the duration (or a resync also stops the deck)
static inline bool trig_combo_busy(const trig_combo_t *tc)
{
    return tc->armed || tc->latched;
}

static inline tc_event_t trig_combo_step(trig_combo_t *tc, bool a, bool b, int frames)
{
    if (a && b) {
        tc->both += (uint32_t)frames;
        if (!tc->armed && !tc->latched && tc->both >= TC_HOLD_FRAMES) {
            tc->armed = true;
            return TC_ARMED;
        }
        return TC_NONE;
    }
    tc->both = 0;
    if (tc->armed) {                    // a gate came up: the beat lands NOW
        tc->armed = false;
        tc->latched = true;             // keep swallowing until both are up
        return TC_FIRE;
    }
    if (tc->latched && !a && !b) tc->latched = false;   // gesture over
    return TC_NONE;
}
