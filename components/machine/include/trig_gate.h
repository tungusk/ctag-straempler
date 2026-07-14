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
} trig_gate_t;

#define TG_LONG_FRAMES ((uint32_t)(0.6f * 44100.0f))

// `down` = gate active (caller maps the active-low trig bit); `frames` = block
// size. Returns at most one event per call — PRESS on the down edge, HOLD at
// the threshold, REL_* on the up edge.
static inline tg_event_t trig_gate_step(trig_gate_t *g, bool down, int frames)
{
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
        return was_long ? TG_REL_LONG : TG_REL_SHORT;
    }
    return TG_NONE;
}

// ---- BOTH-GATE COMBO: the shared RESYNC gesture (Arlo, 2026-07-13) ----------
// Hold TR1 and TR2 together, release, and the machine's beat lands ON the
// release — a manual re-anchor of the PLL that needs no knob. It is a general
// contract, not a deck feature: any clock-following machine can adopt it.
//
//   TC_ARMED — both gates have been down past TC_HOLD_FRAMES (show it in the UI)
//   TC_FIRE  — the first gate came back up: THIS instant is the beat
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
