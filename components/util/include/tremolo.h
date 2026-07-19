#pragma once
#include <stdint.h>
#include <stdbool.h>

// Shared TREMOLO / auto-pan (FX-rack brick, 2026-07-17). One LFO amplitude-
// modulates the output; sine / triangle / square shapes; optional stereo
// antiphase (L/R opposed = auto-pan). Rate is free (Hz) or clock-synced
// (tremolo_set_rate_beats: one LFO cycle per N beats). No slab — zero-init the
// struct. Host writes the param fields directly (the menu clamps); the kernel
// re-clamps. Process IN PLACE on the interleaved int32<<16 stereo buffer.

enum { TREM_SINE = 0, TREM_TRI, TREM_SQR, TREM_NSHAPE };

typedef struct {
    volatile float rate;    // Hz (0.01..30)
    volatile float depth;   // 0..1 (1 = amplitude dips to silence)
    volatile int   shape;   // TREM_*
    volatile bool  stereo;  // true = L/R antiphase (auto-pan)
    volatile bool  sync;    // clock-sync the rate to `div` (fxrack sets it per block)
    volatile int8_t div;    // division index into fxrack_div_beats[] when synced
    float phase;            // 0..1 LFO phase
} tremolo_t;

// one LFO cycle every `beats` quarter-notes at `bpm` -> Hz
static inline void tremolo_set_rate_beats(tremolo_t *t, float beats, float bpm)
{
    if (bpm < 1.0f) bpm = 1.0f;
    if (beats < 0.01f) beats = 0.01f;
    t->rate = bpm / (60.0f * beats);
}

void tremolo_block_i32(tremolo_t *t, int32_t *out, int frames);
// float-scratch worker for the hosted chain (see fxchain.h)
void tremolo_block_f(tremolo_t *t, float *buf, int frames);
