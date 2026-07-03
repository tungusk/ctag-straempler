#include "machine.h"
#include "audio.h"   // machine_sampler lives in components/audio until M0b

// Machines shipped in this firmware, in selector order. This file is the ONLY
// place outside a machine's own component that may name a machine symbol —
// remove a line here and that machine's code drops out of the build entirely.

const machine_t *const machine_registry[] = {
    &machine_sampler,
    NULL,
};
