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
#include "fxrack.h"       // shared FX slot rack (FXK_*, FX_NSLOT_GEN, fxfilter)
#include "cvmtx.h"        // shared CV matrix widget

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
#define IS_MAX_ZONES   8           // multisample: up to 8 mapped samples
#define IS_MAX_FRAMES  1000000u    // ~22.7 s mono resident (2.0 MB, under the 2.1 MB grant)
#define IS_PEAKS       160         // waveform-preview columns (built at load)
#define IS_TAIL_FADE   220         // ~5 ms declick ramp at the end of a one-shot
                                   // (matches the default loop_xfade)
#define IS_SILENCE_LSB 32          // |int16| at/below this counts as silence when
                                   // trimming a fresh sample's loop end (~-60 dBFS)

enum { ENV_IDLE = 0, ENV_ATK, ENV_DEC, ENV_SUS, ENV_REL };   // as Synth
enum { LOOP_OFF = 0, LOOP_FWD };                              // LOOP_PP = v2

// what an auto-tune verdict rests on (inst.tune_src) — surfaced on the Setup
// row so a tuning taken from the FILE NAME is never mistaken for a heard one
enum { TUNE_NONE = 0,   // nothing ran, or nothing usable
       TUNE_AUDIO,      // detected from the audio alone
       TUNE_BOTH,       // detected, with the id's note fixing the octave
       TUNE_NAME,       // nothing pitched heard; the id named the note
       TUNE_CONFLICT }; // detected, but the id claims a different pitch class

// CV matrix destinations — each carries a source (-1 off / 0..7 = CV1..8) and a
// bipolar amount; modulation ADDS to the knob/Setup base per block.
enum { ISM_CUTOFF = 0, ISM_RES, ISM_ENVCUT, ISM_LEVEL,
       ISM_PITCH, ISM_START, ISM_LOOPMOV, ISM_LOOPLEN, ISM_N };

extern const char *const keys_mtx_labels[ISM_N];   // dest names (instsampler.c)

// one mapped sample region (v1 uses zone[0] only)
typedef struct {
    char     sample[24];      // pool id (<=8 chars on disk; 24 for safety)
    int16_t *buf;             // PSRAM-resident audio (mono int16), lazy alloc
    uint32_t cap;             // frames buf can actually hold — NOT always
                              // IS_MAX_FRAMES: the alloc walks a ladder down when
                              // PSRAM is fragmented (see keys_load_zone), so a
                              // shorter sample loads instead of the machine
                              // silently refusing to load anything at all
    uint32_t frames;          // valid frames in buf (0 = empty)
    uint8_t  root;            // MIDI note that plays buf at native rate
    float    fine;            // + cents-as-semitones on top of root (-1..+1);
                              // auto-tune writes the fractional part here
    int16_t  tune_hint;       // note parsed from THIS zone's id (-1 = none). Same
                              // reason as tune_src: the instrument-level one holds
                              // the last LOAD's hint, so after clearing back to one
                              // zone the Auto-Tune row cheerfully showed a hint
                              // belonging to a sample that was no longer loaded.
    int8_t   tune_src;        // TUNE_* for THIS zone. inst.tune_src only ever
                              // holds the LAST load's verdict, so in a
                              // multisample it says nothing about zones 0..n-2 —
                              // and the verdict is exactly what you need when one
                              // zone is out of tune and the others are fine.
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
    int    zone;              // zone LATCHED at note-on. Never re-picked mid-note:
                              // the read cursor is an index into that zone's
                              // buffer, so switching underneath it would jump to
                              // an unrelated point in different audio.
    svf_t  flt;               // per-voice filter (mono)
} is_voice_t;

typedef struct {
    is_zone_t  zone[IS_MAX_ZONES];
    is_voice_t voice[IS_MAX_VOICES];

    // shared instrument params (Setup + knobs)
    int   base_note;          // note the CV1 offset adds to (default 48 = C3)
    bool  quantize;           // snap pitch to semitones
    bool  autotune_load;      // run auto-tune on every fresh sample load
    float tune_hz;            // last auto-tune verdict (0 = none), UI only
    float tune_conf;          // its confidence 0..1, UI only
    int8_t tune_src;          // TUNE_* — what the verdict was based on, UI only
    int16_t tune_hint;        // note parsed out of the sample id (-1 = none)
    int   last_tuned_zone;    // which zone tune_hz/tune_conf actually describe.
                              // Those two are per-LOAD, not per-zone, so without
                              // this the "unsure (X?)" readout attaches the last
                              // load's weak verdict to whatever zone you scroll to.
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
    fxfilter_t filt;          // FX rack filter brick (LP/HP/BP)
    fxfilter_t band;          // FX rack band filter brick (base/width)
    bool  filt_on;            // filter engaged
    // FX rack: which generic effect (FXK_*) runs in slot 0 (FX1) and slot 1 (FX2).
    // Source of truth for on/order; the *_on bools are kept synced from this.
    int8_t fx_slot[FX_NSLOT_GEN];

    // CV matrix (the shared cvmtx widget since 2026-07-20 — identical
    // mechanics to Synth; owns the old cv12_floor conditioning)
    cvmtx_t mtx;

    // four macro knobs w/ takeover (Synth machinery); knob_ctx = -1 -> recapture
    float knob_capt[4];
    bool  knob_live[4];
    int   knob_ctx;

    // live (UI)
    int   cv1_disp;
    float note_disp;          // last played note
    // MULTISAMPLE. One PSRAM ARENA, bump-allocated, NOT one malloc per zone:
    // eight separate 2 MB requests is exactly what fragmented the pool on
    // 2026-07-26 and left Keys unable to load anything at all until a reboot
    // (largest free block 1,998,848 against a 2,000,000 ask). One allocation
    // carved up keeps the single-zone case byte-identical and cannot fragment.
    // Zones are APPEND-ONLY — there is no freeing the middle of an arena — so
    // building a set means clear-then-add, which is also how you think about it.
    int16_t  *arena;
    uint32_t  arena_cap;      // frames the arena can hold
    uint32_t  arena_used;     // frames handed out so far
    int       nzones;         // zones loaded (1 = plain sampler, as before)
    int       edit_zone;      // which zone the Setup rows act on
    volatile bool loading;    // engine plays silent while a zone (re)loads
    char  load_err[24];       // why the last load failed, "" when it succeeded.
                              // A failed load used to be a SILENT no-op, which is
                              // indistinguishable from a broken browser — Arlo hit
                              // exactly that ("i can't seem to load a sample").
    uint8_t peaks[IS_PEAKS];  // waveform preview
} is_state_t;

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline int   clampi(int x, int lo, int hi)         { return x < lo ? lo : x > hi ? hi : x; }

extern is_state_t inst;
extern fxrack_t inst_rk;    // FX rack pointer-view over inst's effect instances

// load a pool sample into zone[0] (mono, resident). 0 ok, <0 fail. Resets the
// loop to the whole sample; caller/preset may then set loop points. Sets/clears
// inst.loading around the SD read.
int keys_load_zone(const char *name);

// MULTISAMPLE. keys_load_zone_at(-1, name) APPENDS a zone; a zi inside the
// existing set only works for the LAST zone (an arena cannot free its middle),
// otherwise it fails with "clear zones first". keys_clear_zones resets the
// arena. keys_zone_for_note picks by NEAREST ROOT — no editable key ranges, so
// auto-tune building a root per zone is all the mapping there is.
int  keys_load_zone_at(int zi, const char *name);
void keys_clear_zones(void);
int  keys_zone_for_note(float note);

// rebuild inst.peaks for the CURRENT edit_zone — call after moving edit_zone,
// or the waveform strip shows one zone's audio under another's loop box
void keys_build_peaks(void);

// drop zones 1..n-1, keep zone 0 (the "Clear Zones" row). Never empties the
// machine — Load Sample is the full reset.
void keys_keep_first_zone(void);

// AUTO-TUNE zone[0]: detect the loaded sample's fundamental (shared
// util/pitch_detect, YIN over the resident PSRAM buffer) and set root + fine so
// it plays in tune. Returns 0 when a confident pitch was found and APPLIED, <0
// when nothing usable was heard (tuning untouched). Either way inst.tune_hz /
// tune_conf carry the verdict for the UI. UI/loader context — ~50 ms, no SD.
int keys_autotune(void);

// snap a frame index to the nearest RISING zero crossing in zone[0]'s buffer
// (declick for loop points). Offline scan — call from the UI/adjust path only.
uint32_t keys_snap_zero(uint32_t frame);

// named patches — usr/keys/PAT_NNN.jsn via the shared preset_store (the #23
// pattern). UI/menu context only.
int keys_patch_save(char *id_out, size_t n);    // mint next id + save; id_out = the id
int keys_patch_load(const char *id);            // load a patch by id (0 ok)
int keys_patch_list(char ids[][12], int max);   // ids newest-first; returns count
