#pragma once
#include <stdint.h>
#include <stdbool.h>

// Offline BPM + beat-grid analysis, lifted verbatim from the deck's proven
// engine (onset-flux autocorrelation with harmonic disambiguation, parabolic +
// long-lag ladder refinement, folded-onset grid phase). DSP ONLY: the caller
// owns the task it runs on, the progress field, adopting the result live, and
// writing the .JSN sidecar (each machine keeps its own v2 writer). This lets
// DoubleDecker stamp an unstamped track — which it otherwise refuses to loop —
// without forking a second copy of the analysis.

#define BPM_HOP     256
#define BPM_ENV_MAX 52000        // ~5 min of track at 44.1k / 256-frame hops

typedef struct {
    float    bpm;    // detected tempo (raw — before any per-caller "feel")
    uint32_t grid;   // first-downbeat offset, in audio frames
    float    conf;   // ACF peak salience, 0..1 (1 - median/peak)
} bpm_result_t;

// Analyse track `id` (resolved via sampfile_f) on the CALLING task. Needs a
// ~6 KB stack + ~208 KB PSRAM for the envelope + a 16 KB internal DMA chunk.
//
//  busy(): return true whenever YOU need the SD bus (playing / loading). The
//    analysis pauses ENTIRELY whenever it is true, so it never touches the card
//    while a ring reader does — this is the "analyse only while stopped" rule.
//    Pass NULL to never pause. For a two-deck caller, have busy() cover BOTH
//    decks so one deck's playback isn't starved while the other analyses.
//  progress: 0..100 written into *progress if non-NULL.
//
// Returns 0 and fills *out on success; <0 on failure (open/probe/OOM, a track
// shorter than ~10 s, or an abort). Aborts promptly if bpm_analyze_abort() is
// called from another task (e.g. the caller's stop()).
int  bpm_analyze(const char *id, bool (*busy)(void), volatile int *progress, bpm_result_t *out);

// Ask a running bpm_analyze() to bail out at its next checkpoint (returns <0).
// Cleared automatically at the start of the next bpm_analyze(). Safe to call
// when nothing is running. Only one analysis runs at a time (one active
// machine), so this needs no handle.
void bpm_analyze_abort(void);
