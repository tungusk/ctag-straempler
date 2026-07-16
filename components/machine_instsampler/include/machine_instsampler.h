#pragma once
#include "machine.h"

// "Keys" — tonal instrument sampler: one PSRAM-resident mono sample played
// melodically by 1V/oct with a sustain loop + ADSR amp/filter. The Synth's
// operability grammar with a real sample voice in place of the oscillator.
extern const machine_t machine_instsampler;
