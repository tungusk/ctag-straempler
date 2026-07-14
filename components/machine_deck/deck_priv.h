#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "clock.h"
#include "svf.h"

// Deck — a tempo-syncing track player. Streams a long usr/*.RAW from SD
// through a PSRAM ring (an unpinned reader task keeps it ahead of the play
// cursor; process() never touches SD) and plays it varispeed, phase-locked
// to the external CV clock:
//
//   rate = (external BPM / track BPM) * (1 + k * phase_error)
//
// The track's BPM and grid (first-downbeat offset) come from an offline
// analysis pass (onset envelope + autocorrelation, deck_analysis.c), cached
// in the track's JSN sidecar so each file is analysed once. The clock
// mult/div setting maps incoming pulses to beats (1/4..4 pulses per beat).

#define DK_RATE        44100
#define DK_RING_FRAMES (DK_RATE * 8)          // 8 s stereo PSRAM ring (~1.4 MB)
#define DK_LOW_WATER   (DK_RATE / 3)          // unmute once ~0.33 s is buffered
                                              // (ring keeps filling to 8 s) —
                                              // low so seeks/scrubs feel instant
#define DK_NAME_LEN    24
#define DK_WF_W        144            // waveform thumbnail columns

// analysis envelope: 256-frame hops (~172 Hz) — resolves ~±1 BPM at 120
#define DK_HOP         256
#define DK_ENV_MAX     (52000)                // ~5 min of track

enum { DK_AN_IDLE = 0, DK_AN_RUNNING, DK_AN_DONE, DK_AN_FAIL };

typedef struct {
    // streaming
    int16_t *ring;                 // PSRAM, DK_RING_FRAMES * 2
    volatile uint32_t wpos;        // ring write head (frames, absolute)
    volatile uint32_t rpos_i;      // play cursor, integer part (absolute frames)
    double   rpos_f;               // fractional part (engine only)
    volatile uint32_t file_frames; // track length
    volatile bool playing;
    volatile bool loading;         // reader (re)filling after seek/track change
    // Seek protocol: requesters (UI task, audio task) ONLY set loading +
    // seek_to + seek_req. The READER applies them — including writing
    // rpos_i — after a short settle so the engine has parked on `loading`.
    // (Concurrent rpos writes from UI scrubs racing the audio task's
    // loop-restart caused garbage playback + stuck buffering.)
    volatile uint32_t seek_to;
    volatile bool seek_req;
    char track[DK_NAME_LEN];       // loaded track id ("" = none)

    // grid + sync
    volatile float track_bpm;      // EFFECTIVE bpm (bpm_raw x feel; 0 = unknown)
    volatile float bpm_raw;        // sidecar/analysis bpm before feel
    volatile float feel;           // per-TRACK tempo feel x0.5/1/2 — harmonic
                                   // ambiguity is TASTE, not error ("some
                                   // tracks sound right at .5"); persisted in
                                   // the sidecar as "feel"
    volatile float clk_scale;      // Setup: x0.5/1/2 on the incoming clock —
                                   // the friendly layer over ppb
    volatile uint32_t grid_offset; // first downbeat, frames into the file
    volatile bool sync;            // follow the external clock
    volatile bool loop;            // wrap to the downbeat at end of file
    volatile int  clk_src;         // CV channel of the clock (default CV8)
    volatile int  ppb_idx;         // pulses-per-beat index into dk_ppb[] (mult/div)
    volatile int  pitch_cv;        // knob7 free-rate when sync is off
    // conditioned clock input — the shared front-end (clock.h): floor-tracked
    // Schmitt, ppb-scaled sanity gates, ghost gate, raw-fire diagnostics.
    // The deck's private copy is what clockin_t was extracted from.
    clockin_t ci;
    float rate_sm;                 // smoothed rate (edge jitter -> no warble)
    volatile uint32_t dbg_starve;  // blocks muted mid-play: reader fell behind
    volatile float rate;           // current playback rate (UI display)
    volatile float phase_err;      // current beat phase error (UI display)
    volatile float phase_int;      // PLL integrator: nulls residual frequency
                                   // error (imperfect track_bpm) -> no drift
    volatile float phase_offset;   // NUDGE: manual phase trim the loop locks to
                                   // (pulse-phase units [0,1); the loop holds it)
    volatile float sync_slew;      // SYNC catch-up: frames left to shift via a
                                   // brief rate bend (no seek/dropout); 0 = idle
    volatile float speed_mult;     // knob7 while synced: x0.5 / x1 / x2, still locked

    // KO-II buffer LOOP (convergence S6-S7). Engage is SEAMLESS: the window
    // anchors on the nearest grid beat (ring-resident by the reader lead cap)
    // and the first audible effect is the first wrap — no seek, no mute.
    // Protocol amendment: while loop_active && !loading && !seek_req the
    // ENGINE wraps rpos_i back by whole windows; any seek/load re-parks it
    // (the reader stays the only seek-writer).
    volatile bool loop_active;
    volatile bool loop_freeze;     // Setup (persisted): release resumes at the
                                   // loop (freeze) vs where playback WOULD be
    volatile uint32_t loop_start;  // window start (frames, absolute)
    volatile uint32_t loop_len_fr; // window length (frames)
    volatile uint32_t loop_adv;    // frames advanced while looping — feeds the
                                   // keeps-running release phantom (robust
                                   // across wraps, len changes, start moves)
    volatile int  loop_len_beats;  // display + PLL whole-pulse invariant
    uint32_t engage_rpos;          // rpos at engage (phantom base)
    // SEAM CROSSFADE (Arlo, ear test: "faint click at the seam"). At the wrap
    // the incoming head blends against the OUTGOING TAIL CONTINUING PAST the
    // window end — not against itself. That keeps the loop period exactly N
    // beats (the PLL never sees a hiccup: a fade built from the head would
    // shorten every cycle by the fade length). The material past the window is
    // ring-resident by the reader's +4096 park overshoot.
    uint32_t xf_left;              // frames of fade remaining (0 = none)
    uint32_t xf_src;               // absolute frame of the outgoing tail

    // DJ filter (knob6): centre = bypass, left = LP sweeping down,
    // right = HP sweeping up. Chamberlin SVF, one per channel.
    volatile int filt_cv;          // raw knob (UI display)
    float flt_f;                   // slewed coefficient
    float out_gain;                // declick ramp (0..1) across seeks/stops
    svf_t flt_l, flt_r;            // SVF state (util/svf.h — shared kernel)
    volatile int flt_mode;         // 0 off, 1 LP, 2 HP (UI display)

    // waveform thumbnail (sampler3 scheme): the reader builds one decimated
    // column per idle pass once the ring is warm — never blocks playback
    uint8_t wf[144];               // per-column peak, 0..255 (DK_WF_W)
    volatile int wf_state;         // 0 none, 1 building, 2 valid
    int wf_col;                    // reader progress

    // analysis
    volatile int an_state;         // DK_AN_*
    volatile int an_progress;      // 0..100
    volatile bool auto_an;         // Setup toggle: auto-analyze unanalyzed loads
    volatile bool an_auto_req;     // auto-analyze queued behind a running one
    volatile float an_bpm;         // result before commit
    volatile uint32_t an_grid;
    volatile float an_conf;        // ACF peak salience 0..1 (1 - median/peak)
} dk_state_t;

extern dk_state_t dk;
extern const float dk_ppb[6];     // {0.25, 0.5, 1, 2, 4, 8} pulses per beat
// effective pulses per beat: the mult/div setting over the clock scale —
// keep the three tempo layers distinct: feel (per-track, persisted) x
// clk_scale (setup) x speed_mult (performance)
#define DK_PPB_EFF() (dk_ppb[dk.ppb_idx] / (dk.clk_scale > 0 ? dk.clk_scale : 1.0f))
extern const char *const dk_ppb_names[6];

// UI-side (SD-touching; call from UI/background tasks only)
int  deck_load_track(const char *name);   // select + start streaming + read sidecar
void deck_toggle_play(void);              // play/pause (resumes at the cue point)
void deck_restart(void);                  // jump to the downbeat
void deck_seek_beats(int beats);          // grid-snapped scrub (± whole beats)
void deck_sync_now(void);                 // hard-snap grid phase to the clock now
void deck_set_feel(float f);              // x0.5/1/2; reapplies bpm + writes sidecar
int  deck_analyze_start(void);            // spawn BPM/grid analysis of the track
void deck_analysis_commit(void);          // adopt an_bpm/an_grid + write sidecar
