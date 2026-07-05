#include "machine.h"
#include "machine_sampler.h"
#include "machine_looper.h"
#include "machine_slicer.h"

// declared bare: machine_sampler2.h drags the fork's whole type universe,
// which collides with machine_sampler.h's copy inside a single TU
extern const machine_t s2_machine_sampler;

// Machines shipped in this firmware, in selector order. This file is the ONLY
// place outside a machine's own component that may name a machine symbol —
// remove a line here and that machine's code drops out of the build entirely.

extern const machine_t machine_stub;   // main/machine_stub.c

const machine_t *const machine_registry[] = {
    &machine_sampler,
    &s2_machine_sampler,
    &machine_looper,
    &machine_slicer,
    &machine_stub,
    NULL,
};
