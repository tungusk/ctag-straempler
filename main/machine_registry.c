#include "machine.h"
#include "machine_sampler3.h"
#include "machine_looper.h"
#include "machine_slicer.h"
#include "machine_granular.h"
#include "machine_glitch.h"
#include "machine_drumsampler.h"
#include "machine_freesound.h"
#include "machine_deck.h"
#include "machine_dualdeck.h"
#include "machine_tracker.h"
#include "machine_radio.h"
#include "machine_synth.h"
#include "machine_instsampler.h"
#include "machine_tape.h"
#include "machine_editor.h"

// NOTE: the legacy "Sampler2" fork (s2_machine_sampler, components/machine_sampler2)
// was pulled from the build 2026-07-18 — sampler3 ("Sampler") has the hardware
// verdict. The code stays in the repo but machine_sampler2 is now in
// EXCLUDE_COMPONENTS (top-level CMakeLists.txt) so it no longer compiles or
// links. Re-add it here + drop it from EXCLUDE_COMPONENTS to bring it back.

// Machines shipped in this firmware, in selector order. This file is the ONLY
// place outside a machine's own component that may name a machine symbol —
// remove a line here and that machine's code drops out of the build entirely.

extern const machine_t machine_stub;   // main/machine_stub.c

const machine_t *const machine_registry[] = {
    &machine_sampler3,       // "Sampler" — the deck-architecture rebuild
    &machine_looper,
    &machine_slicer,
    &machine_granular,
    &machine_glitch,
    &machine_drumsampler,
    &machine_deck,
    &machine_dualdeck,     // "DoubleDecker" — clock-locked track blender
    &machine_tracker,
    &machine_freesound,
    &machine_radio,        // "Radio" — icecast/shoutcast MP3 streamer
    &machine_synth,        // "Synth" — no-sample subtractive voice
    &machine_instsampler,  // "Keys" — tonal instrument sampler (pitched + sustain loop)
    &machine_tape,         // "Tape" — single-track tape recorder/editor (big-wave UI, FX-in-path)
    &machine_editor,       // "Editor" — offline file->file sample ops
    &machine_stub,
    NULL,
};
