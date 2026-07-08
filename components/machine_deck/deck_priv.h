#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "clock.h"

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
    volatile float track_bpm;      // from analysis / manual override (0 = unknown)
    volatile uint32_t grid_offset; // first downbeat, frames into the file
    volatile bool sync;            // follow the external clock
    volatile bool loop;            // wrap to the downbeat at end of file
    volatile int  clk_src;         // CV channel of the clock (default CV8)
    volatile int  ppb_idx;         // pulses-per-beat index into dk_ppb[] (mult/div)
    volatile int  pitch_cv;        // knob7 free-rate when sync is off
    beatclock_t clk;
    // clock-input conditioning: beatclock's fixed thresholds (1500/800 abs)
    // misfire on attenuated/offset channels (this unit's CV8 caps at ~half);
    // a floor-tracked Schmitt synthesises a clean square for it instead
    int  clk_base;                 // tracked floor of the clock channel
    bool clk_high;                 // Schmitt state
    float rate_sm;                 // smoothed rate (edge jitter -> no warble)
    // clock diagnostics (surfaced via /status): raw Schmitt fires + spacing
    volatile uint32_t dbg_edges;
    volatile uint32_t dbg_iv;      // frames between the last two fires
    uint32_t dbg_since;
    volatile uint32_t dbg_starve;  // blocks muted mid-play: reader fell behind
    volatile float rate;           // current playback rate (UI display)
    volatile float phase_err;      // current beat phase error (UI display)
    volatile float speed_mult;     // knob7 while synced: x0.5 / x1 / x2, still locked

    // DJ filter (knob6): centre = bypass, left = LP sweeping down,
    // right = HP sweeping up. Chamberlin SVF, one per channel.
    volatile int filt_cv;          // raw knob (UI display)
    float flt_f;                   // slewed coefficient
    float out_gain;                // declick ramp (0..1) across seeks/stops
    float lp_l, bp_l, lp_r, bp_r;  // SVF state
    volatile int flt_mode;         // 0 off, 1 LP, 2 HP (UI display)

    // analysis
    volatile int an_state;         // DK_AN_*
    volatile int an_progress;      // 0..100
    volatile float an_bpm;         // result before commit
    volatile uint32_t an_grid;
} dk_state_t;

extern dk_state_t dk;
extern const float dk_ppb[5];     // {0.25, 0.5, 1, 2, 4} pulses per beat
extern const char *const dk_ppb_names[5];

// UI-side (SD-touching; call from UI/background tasks only)
int  deck_load_track(const char *name);   // select + start streaming + read sidecar
void deck_toggle_play(void);              // play/pause (resumes at the cue point)
void deck_restart(void);                  // jump to the downbeat
void deck_seek_beats(int beats);          // grid-snapped scrub (± whole beats)
int  deck_analyze_start(void);            // spawn BPM/grid analysis of the track
void deck_analysis_commit(void);          // adopt an_bpm/an_grid + write sidecar
