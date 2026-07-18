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
#include "fxfilter.h"

// FX rack (curated slots). FX1/FX2 each hold one GENERIC effect (or OFF) and run
// in slot order; FX3 is the fixed reverb slot. Reordering FX1<->FX2 changes the
// chain order. Spike lives in Keys; extract to components/fxrack for Synth/Tape.
enum { FXK_OFF = 0, FXK_OD, FXK_FLG, FXK_TREM, FXK_DLY, FXK_FILT, FXK_NGEN };
#define FX_NSLOT_GEN 2

// Keys — tonal instrument sampler (see machine_instsampler.h). v1: one mono
// PSRAM-resident sample, varispeed-pitched across the keyboard from CV1
// (1V/oct), a forward SUSTAIN LOOP so held notes never run out, the Synth's
// linear ADSR + env-opened SVF, four takeover macro knobs, and an 8-dest CV
// matrix. Multi-zone key splits, streaming, and 2-voice paraphony are v2 (the
// state is factored into voice[]/zone[] arrays so those are additive).

#define IS_RATE        44100
#define IS_CV1_ZERO    877         // ch1 idle -> 0 semitones (measured, as Synth)
#define IS_CTS_PER_ST  49.0f       // ADC counts per semitone (sampler2 LUT scale)
#define IS_MAX_VOICES  1           // v1 mono; bump to 2 for #28 paraphony
#define IS_MAX_ZONES   1           // v1 single zone; v2 -> 6/8
#define IS_MAX_FRAMES  1000000u    // ~22.7 s mono resident (2.0 MB, under the 2.1 MB grant)
#define IS_PEAKS       160         // waveform-preview columns (built at load)

enum { ENV_IDLE = 0, ENV_ATK, ENV_DEC, ENV_SUS, ENV_REL };   // as Synth
enum { LOOP_OFF = 0, LOOP_FWD };                              // LOOP_PP = v2

// CV matrix destinations — each carries a source (-1 off / 0..7 = CV1..8) and a
// bipolar amount; modulation ADDS to the knob/Setup base per block.
enum { ISM_CUTOFF = 0, ISM_RES, ISM_ENVCUT, ISM_LEVEL,
       ISM_PITCH, ISM_START, ISM_LOOPMOV, ISM_LOOPLEN, ISM_N };

// one mapped sample region (v1 uses zone[0] only)
typedef struct {
    char     sample[24];      // pool id (<=8 chars on disk; 24 for safety)
    int16_t *buf;             // PSRAM-resident audio (mono int16), lazy alloc
    uint32_t frames;          // valid frames in buf (0 = empty)
    uint8_t  root;            // MIDI note that plays buf at native rate
    uint8_t  loop_mode;       // LOOP_OFF / LOOP_FWD
    uint32_t loop_start, loop_end;
    uint32_t loop_xfade;      // crossfade frames at the wrap seam (0 = hard loop)
} is_zone_t;

// per-note voice state (v1: one; the array leaves room for #28 paraphony)
typedef struct {
    bool   active;
    double pos;               // fractional read cursor (frames)
    int    env_stage;
    float  env;               // 0..1
    float  cur_note;          // glide-slewed note actually sounding
    bool   gate;              // last gate level
    svf_t  flt;               // per-voice filter (mono)
} is_voice_t;

typedef struct {
    is_zone_t  zone[IS_MAX_ZONES];
    is_voice_t voice[IS_MAX_VOICES];

    // shared instrument params (Setup + knobs)
    int   base_note;          // note the CV1 offset adds to (default 48 = C3)
    bool  quantize;           // snap pitch to semitones
    float atk, dec, sus, rel; // ADSR (Synth semantics)
    float env_to_cut;         // 0..1
    float cutoff_base;        // Hz (K6)
    float res01;              // 0..1 (K7)
    float glide;              // portamento seconds (0 = off)
    float level;              // master 0..1
    float start_frac;         // K5 note-on start offset 0..1
    reverb_t rv;              // output reverb (lazy PSRAM slab; RV_OFF = bypass)
    fxdelay_t dly;            // output delay (lazy PSRAM slab; runs delay->reverb)
    bool  dly_on;             // delay engaged (slab stays allocated once inited)
    overdrive_t od;           // output overdrive (no slab; zero-init)
    bool  od_on;              // overdrive engaged
    flanger_t flg;            // output flanger (lazy PSRAM slab; mirrors dly)
    bool  flg_on;             // flanger engaged (slab stays allocated once inited)
    tremolo_t trem;           // output tremolo (no slab; zero-init)
    bool  trem_on;            // tremolo engaged
    fxfilter_t filt;          // FX rack filter brick (insert; not the voice svf)
    bool  filt_on;            // filter engaged
    // FX rack: which generic effect (FXK_*) runs in slot 0 (FX1) and slot 1 (FX2).
    // Source of truth for on/order; the *_on bools are kept synced from this.
    int8_t fx_slot[FX_NSLOT_GEN];

    // CV matrix (identical mechanics to Synth)
    int8_t mtx_src[ISM_N];    // -1 off / 0..7 = CV1..8
    float  mtx_amt[ISM_N];    // -1..+1
    int    cv12_floor[2];     // tracked idle floor for ch1/2

    // four macro knobs w/ takeover (Synth machinery); knob_ctx = -1 -> recapture
    float knob_capt[4];
    bool  knob_live[4];
    int   knob_ctx;

    // live (UI)
    int   cv1_disp;
    float note_disp;          // last played note
    volatile bool loading;    // engine plays silent while a zone (re)loads
    uint8_t peaks[IS_PEAKS];  // waveform preview
} is_state_t;

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline int   clampi(int x, int lo, int hi)         { return x < lo ? lo : x > hi ? hi : x; }

extern is_state_t inst;

// load a pool sample into zone[0] (mono, resident). 0 ok, <0 fail. Resets the
// loop to the whole sample; caller/preset may then set loop points. Sets/clears
// inst.loading around the SD read.
int keys_load_zone(const char *name);

// snap a frame index to the nearest RISING zero crossing in zone[0]'s buffer
// (declick for loop points). Offline scan — call from the UI/adjust path only.
uint32_t keys_snap_zero(uint32_t frame);

// named patches — usr/keys/PAT_NNN.jsn via the shared preset_store (the #23
// pattern). UI/menu context only.
int keys_patch_save(char *id_out, size_t n);    // mint next id + save; id_out = the id
int keys_patch_load(const char *id);            // load a patch by id (0 ok)
int keys_patch_list(char ids[][12], int max);   // ids newest-first; returns count
