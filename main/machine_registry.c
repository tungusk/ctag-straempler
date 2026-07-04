#include "machine.h"
#include "machine_sampler.h"

// Machines shipped in this firmware, in selector order. This file is the ONLY
// place outside a machine's own component that may name a machine symbol —
// remove a line here and that machine's code drops out of the build entirely.

extern const machine_t machine_stub;   // main/machine_stub.c

const machine_t *const machine_registry[] = {
    &machine_sampler,
    &machine_stub,
    NULL,
};
