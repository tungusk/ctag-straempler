#include "machine.h"
#include "machine_sampler.h"
#include "machine_sampler3.h"
#include "machine_looper.h"
#include "machine_slicer.h"
#include "machine_granular.h"
#include "machine_glitch.h"
#include "machine_drumsampler.h"
#include "machine_freesound.h"
#include "machine_deck.h"
#include "machine_tracker.h"

// declared bare: machine_sampler2.h drags the fork's whole type universe,
// which collides with machine_sampler.h's copy inside a single TU
extern const machine_t s2_machine_sampler;

// Machines shipped in this firmware, in selector order. This file is the ONLY
// place outside a machine's own component that may name a machine symbol —
// remove a line here and that machine's code drops out of the build entirely.

extern const machine_t machine_stub;   // main/machine_stub.c

const machine_t *const machine_registry[] = {
    &machine_sampler,        // "Sampler0" — frozen original (removal pending)
    &s2_machine_sampler,     // "Sampler2" — HIDDEN fallback until sampler3 is hw-verified
    &machine_sampler3,       // "Sampler" — the deck-architecture rebuild
    &machine_looper,
    &machine_slicer,
    &machine_granular,
    &machine_glitch,
    &machine_drumsampler,
    &machine_deck,
    &machine_tracker,
    &machine_freesound,
    &machine_stub,
    NULL,
};
