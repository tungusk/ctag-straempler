#include "machine.h"

// Machines compiled into this firmware, in selector order. A build ships any
// subset by editing this table — nothing in core references a machine
// directly, so removing an entry removes its code entirely.
//
// M0a: table is empty while the sampler is being extracted from the core;
// machine_sampler joins here in M0b.

const machine_t *const machine_registry[] = {
    NULL,
};
