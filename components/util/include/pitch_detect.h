#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Monophonic PITCH DETECTION over an int16 mono buffer — the DSP half shared by
// Keys' sample auto-tune and (next) the live line-in tuner, factored out the
// same way bpm_analysis.c was lifted from the deck.
//
// Engine: YIN (difference function -> cumulative mean normalized difference ->
// absolute threshold -> parabolic interpolation), run COARSE on a 4x decimated
// copy of the window (cheap 27.5 Hz..1.5 kHz search) and then REFINED at the
// native rate around the coarse lag, which buys sub-cent accuracy for a few
// thousand extra multiplies. pitch_detect_buf() scans several windows across the
// sample and takes the median of the confident ones, so an attack transient or a
// silent tail can't set the verdict alone.
//
// Pure DSP: no SD, no globals, no task assumptions — but it is NOT audio-task
// work (a full scan is a few hundred ms). Call it from a UI/loader/analysis
// task, as Keys does after a sample load.

typedef struct {
    float hz;      // detected fundamental in Hz (0 = nothing found)
    float conf;    // 0..1 confidence (YIN salience x cross-window agreement)
    int   midi;    // nearest MIDI note (69 = A4 = 440 Hz)
    float cents;   // offset from that note, -50..+50
} pitch_result_t;

// Scratch the window worker needs (~6 KB). Allocate once and reuse it for a
// repeating caller (the tuner); pitch_detect_buf() does its own.
size_t pitch_scratch_bytes(void);

// One window: analyse exactly `n` frames starting at `buf` (n >= 2048 for a
// usable low end; more is better). `scratch` must be pitch_scratch_bytes() big,
// or NULL to malloc/free internally. Returns 0 and fills *out on success, <0 if
// the window is silent, aperiodic, or scratch could not be allocated.
int pitch_detect_window(const int16_t *buf, uint32_t n, float rate,
                        void *scratch, pitch_result_t *out);

// Whole sample: up to 6 windows spread across the middle of the buffer, silent
// ones skipped, median of the confident results. `conf` folds in how well those
// windows agreed, so a chord or a noise hit scores low instead of lying.
// Returns 0 / <0 as above.
int pitch_detect_buf(const int16_t *buf, uint32_t frames, float rate,
                     pitch_result_t *out);

// Read a note name out of a SAMPLE ID ("EP_C4", "PNOF#3", "BASSA2", "AB3") and
// return it as a MIDI note, or -1 if the id carries none. Needs an explicit
// octave digit ending the token, so a bare "PIANO" reads as no hint.
//
// *isolated (optional) reports how much to trust it: true when the note letter
// does NOT sit inside a word ("EP_C4" -> C4, isolated), false when it might just
// be the tail of one ("TAPE4" -> E4, NOT isolated; "PNOC4" -> C4, also not).
// The caller is expected to lean on a non-isolated hint only where audio already
// agrees — see keys_autotune(), which uses a matching hint to fix the OCTAVE
// (what detection actually gets wrong) and never the pitch class.
int pitch_name_hint(const char *id, bool *isolated);

// Hz -> midi/cents (fills those fields of *out; leaves hz/conf alone).
void pitch_from_hz(float hz, pitch_result_t *out);

// "A#3" style name for a MIDI note (MIDI 60 = C4). Safe for any int.
void pitch_note_name(int midi, char *out, size_t n);
