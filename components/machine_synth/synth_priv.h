#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "svf.h"

// Synth voice — a no-sample sound source. v1 is a monophonic subtractive voice:
// polyBLEP saw<->square oscillator, 1V/oct pitch on CV1, TR1 gate -> linear ADSR
// amp envelope, SVF low-pass (knob6 cutoff, knob7 resonance) with an env->cutoff
// amount. Reuses util/svf. Later: FM + wavetable engines, polyphony, glide.

#define SY_RATE  44100

// 1V/oct on THIS unit: sampler2 measured ~49 ADC counts / semitone (its pitch
// LUT indexes (adc-1424)/49). We reuse that scale, but zero the offset at the
// ch1 idle (~877) so an UNPATCHED jack plays exactly the base note.
#define SY_CV1_ZERO   877
#define SY_CTS_PER_ST 49.0f

enum { ENV_IDLE = 0, ENV_ATK, ENV_DEC, ENV_SUS, ENV_REL };

enum { ENG_VA = 0, ENG_FM };     // oscillator engine
enum { LFO_OFF = 0, LFO_CUT, LFO_PITCH };   // LFO destination

typedef struct {
    // performance state
    float phase;                 // oscillator (VA) / carrier (FM) phase 0..1
    float mphase;                // FM modulator phase 0..1
    float lfo_phase;             // LFO phase 0..1
    int   env_stage;
    float env;                   // envelope level 0..1
    float freq;                  // TARGET note frequency (Hz)
    float cur_freq;              // glide-slewed frequency actually sounding
    bool  gate;                  // last gate level
    svf_t flt_l;                 // (mono voice, one filter; L used, mirrored to R)

    // params (Setup + knobs)
    int   engine;                // ENG_VA / ENG_FM
    int   base_note;             // MIDI note the CV1 offset is added to (default 48 = C3)
    bool  quantize;              // snap pitch to semitones
    float shape;                 // VA: 0 = saw, 1 = square (morph)
    float fm_ratio;              // FM: modulator:carrier frequency ratio
    float fm_index;              // FM: modulation index (depth), scaled by the envelope
    float atk, dec, sus, rel;    // ADSR: times in seconds, sustain 0..1
    float env_to_cut;            // 0..1 envelope -> cutoff amount
    float glide;                 // portamento time in seconds (0 = off)
    float lfo_rate;              // LFO Hz
    float lfo_depth;             // 0..1
    int   lfo_dest;              // LFO_OFF / LFO_CUT / LFO_PITCH
    float level;                 // master 0..1

    // live (from knobs, per block)
    float cutoff_base;           // Hz from knob6
    float res01;                 // 0..1 from knob7
    int   cv1_disp;              // last CV1 read (UI)
} sy_state_t;

extern sy_state_t sy;
