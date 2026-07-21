#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "svf.h"
#include "reverb.h"
#include "fxdelay.h"
#include "overdrive.h"
#include "flanger.h"
#include "tremolo.h"
#include "fxrack.h"       // shared FX slot rack (pulls fxfilter.h)
#include "cvmtx.h"        // shared CV matrix widget

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

enum { ENG_VA = 0, ENG_FM, ENG_WT };     // oscillator engine (VA / FM / wavetable)
enum { LFO_OFF = 0, LFO_CUT, LFO_PITCH };   // LFO destination

// CV matrix destinations — each carries its own source (-1 off / 0..7 = CV1..8)
// and a bipolar amount; the modulation ADDS to the knob/Setup base per block.
enum { SYM_CUTOFF = 0, SYM_RES, SYM_TIMBRE, SYM_ENVCUT,
       SYM_LFORATE, SYM_LFODEPTH, SYM_LEVEL, SYM_PITCH, SYM_N };

#define SY_WT_MAX 4096           // wavetable cap (a single-cycle wave is 256..2048)

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

    int16_t *wave;               // wavetable buffer (PSRAM, lazy-alloc)
    int      wave_len;           // samples in the wavetable (0 = none loaded)
    char     wave_name[24];      // loaded wave id (for the preset + UI)

    reverb_t rv;                 // output reverb (lazy PSRAM slab; RV_OFF = bypass)
    fxdelay_t dly;               // output delay (lazy PSRAM slab; runs delay->reverb)
    bool  dly_on;                // delay engaged (slab stays allocated once inited)
    overdrive_t od;              // output overdrive (no slab; part of zero-init sy)
    bool  od_on;                 // overdrive engaged
    flanger_t flg;               // output flanger (lazy PSRAM slab; like dly)
    bool  flg_on;                // flanger engaged (slab stays allocated once inited)
    tremolo_t trem;              // output tremolo (no slab; part of zero-init sy)
    bool  trem_on;               // tremolo engaged
    fxfilter_t filt;             // FX rack filter brick (LP/HP/BP)
    fxfilter_t band;             // FX rack band filter brick (base/width)
    int8_t fx_slot[FX_NSLOT_GEN]; // FX rack: generic slot assignment (FX1,FX2)

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
    float cutoff_base;           // Hz from knob6 (all engines)
    float res01;                 // 0..1 resonance (knob7 in VA)
    float fold;                  // 0..1 wavefold (knob7 in WT)
    int   cv1_disp;              // last CV1 read (UI)

    // FOUR macro knobs (ch5..8 = K5..K8): K5 timbre (engine-aware), K6 cutoff,
    // K7 resonance, K8 env->cut. Built units have all four; this dev unit's K5/K8
    // are weak, so each uses drums-style TAKEOVER: the current value (Setup or
    // default) holds until the knob is actually moved, then the knob drives it.
    float knob_capt[4];          // captured position per knob at the last (re)capture
    bool  knob_live[4];          // knob has moved past threshold -> it drives its param
    int   knob_engine;           // engine the captures are valid for (-1 = recapture)

    // CV matrix (the shared cvmtx widget since 2026-07-20; adds on top of
    // base — dest meanings/scales in synth.c's apply switch). Owns the
    // ch1/2 floor conditioning that used to live here as cv12_floor.
    cvmtx_t mtx;
} sy_state_t;

extern const char *const synth_mtx_labels[SYM_N];   // dest names (synth.c)

extern sy_state_t sy;
extern fxrack_t sy_rk;    // FX rack pointer-view over sy's effect instances

// load a pool sample as the wavetable (mono, up to SY_WT_MAX). Does NOT change
// the engine — the caller (menu) selects ENG_WT on success. 0 ok, <0 fail.
int synth_load_wave(const char *name);

// named patches — usr/synth/PAT_NNN.jsn, one file per patch (see synth.c).
int synth_patch_save(char *id_out, size_t n);   // mint next id + save; id_out = the id
int synth_patch_load(const char *id);           // load a patch by id (0 ok)
int synth_patch_list(char ids[][12], int max);  // ids newest-first; returns count
