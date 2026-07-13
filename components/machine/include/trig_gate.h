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
