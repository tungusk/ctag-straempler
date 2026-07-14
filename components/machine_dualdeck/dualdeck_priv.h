#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "clock.h"
#include "svf.h"
#include "sampfile.h"

// Dual-deck — a clock-locked track BLENDER, not a DJ rig (the design reframe:
// manual beatmatching is what eats controls; here both decks phase-lock to the
// SAME conditioned clock, so they are beatmatched by construction and the
// performer's verbs shrink to fit the panel):
//
//   TR1 = FOCUSED deck transport: tap = quantized start/restart (next BAR),
//        hold = quantized stop  (unified TR grammar, convergence era)
//   TR2 = FOCUSED deck LOOP: press = toggle (beat-true), hold = momentary
//   NOTE the trade: trigs address the focused deck (encoder picks), so a
//   sequencer can't independently gate both decks — grammar consistency won
//   knob6/CV6 = master DJ filter on the summed mix (the deck's LP/HP sweep) —
//               the sweep lives on CV6 in every machine; consistency wins
//   knob7/CV7 = equal-power crossfade A<->B (takeover fade auto-moves it on a
//               deck start; moving the knob past a deadband grabs it back)
//   encoder   = focus (turn) / track browser for the focused deck (press) /
//               Setup (long)
//
// Each deck is a dk-style streaming voice: PSRAM ring, ONE shared reader task
// owning all SD I/O, per-deck PLL against the shared clockin_t. A stopped deck
// PARKS AT ITS CUE (grid downbeat) with the ring pre-filled, so a quantized
// start is a flag flip — zero SD wait on the bar boundary.
// Tempo truth comes from the track's sidecar stamp (deck-analyzed or sampler3
// clock-stamped takes); there is NO analysis engine in this machine. An
// unstamped track shows "no grid" and free-runs at unity.

#define DD_RATE        44100
#define DD_RING_FRAMES (DD_RATE * 6)          // 6 s stereo PSRAM ring x2 (~2.1 MB)
#define DD_LOW_WATER   (DD_RATE / 3)
#define DD_NAME_LEN    24
#define DD_WF_W        120                    // waveform columns per deck
enum { DD_LAY_V = 0, DD_LAY_H = 1 };   // stacked single-decks / side-by-side panels
enum { DD_KNOB_CTX = 0, DD_KNOB_FIXED = 1 };   // contextual knobs / the explicit CV Map

typedef struct {
    // streaming — PLAYBACK-ORDER frame space (the deck's 2026-07-13 loop
    // rework, transplanted). wpos/rpos are monotonic PLAYBACK counters, not
    // file frames: the READER owns the playback->file mapping, so a loop is a
    // MAPPING (not a cursor wrap) and its length is bounded by the TRACK, not
    // by the ring.
    //
    //   file_frame(p) = loop_active ? loop_start + ((p - map_p0) % loop_len_fr)
    //                               : map_f0 + (p - map_p0)
    //
    // Ring slot is p % DD_RING_FRAMES, so counters are NEVER rebased — every
    // transition rewrites the MAPPING instead.
    int16_t *ring;                 // PSRAM, DD_RING_FRAMES * 2
    volatile uint32_t wpos;
    volatile uint32_t rpos_i;
    double   rpos_f;
    volatile uint32_t file_frames;
    volatile bool playing;
    volatile bool loading;         // reader (re)filling after seek/track change
    volatile uint32_t seek_to;     // seek protocol: requesters set flags only,
    volatile bool seek_req;        // the READER applies them (deck lesson)
    volatile bool track_req;
    sampfile_t sf;                 // container descriptor (reader-owned)
    char pending[DD_NAME_LEN];
    char track[DD_NAME_LEN];       // loaded track id ("" = none)
    volatile uint32_t map_p0, map_f0;   // mapping origin (playback / file)
    volatile uint32_t ui_fpos;     // published FILE position of the play cursor
    volatile uint32_t ui_lstart, ui_llen;   // the window the UI should DRAW
                                            // (pending one if a move is scheduled)

    // grid + sync (sidecar stamp; no analysis in this machine)
    volatile float track_bpm;      // 0 = unstamped -> free-run "no grid"
    volatile uint32_t grid_offset;

    // quantized transport: armed ops fire on the next bar boundary
    volatile bool arm_start;       // start/restart from the cue
    volatile bool arm_stop;        // stop + re-park at the cue

    // TRACK LOOP as a streamed window (deck parity): the reader wraps its own
    // reads at the last WHOLE BEAT instead of seeking to the cue at EOF. The
    // old EOF seek muted, re-buffered and RESET the PLL every pass — the deck's
    // "it re-detects tempo at the beginning of every cycle".
    volatile uint32_t tl_start;    // = grid_offset
    volatile uint32_t tl_len;      // whole beats of track (0 = disabled)

    // per-deck KO-II LOOP — STREAMED (deck rework, transplanted): TR2 press =
    // toggle on the FOCUSED deck (unified TR grammar). Engage anchors on the
    // grid beat AT OR BEFORE the cursor and only rewrites the MAPPING; the
    // READER wraps its own file reads at the window end and crossfades the
    // seam, so the window can be ANY length (1/4 .. 256 beats) — the ring
    // stopped being the ceiling. Release = rebase to linear, no duck.
    volatile bool loop_active;
    volatile uint32_t loop_start;  // window start (FILE frames)
    volatile uint32_t loop_len_fr; // window length (frames) — any length
    volatile int  loop_beats;      // display, in QUARTER-beats
    // a window move/resize is SCHEDULED at the reader's frontier: buffered audio
    // plays out, the reader starts the new window, the cursor commits on arrival.
    // Truncating the read-ahead instead starved the ring — and a starve freezes
    // the cursor while the clock runs on, which is a PHASE SLIP, not a dropout.
    volatile uint32_t rm_start, rm_len, rm_p0, rm_at;   // rm_at 0 = none

    // PLL (deck math, per deck, against the shared clock)
    volatile float phase_offset;   // the LOCK POINT — moved by the both-trig
                                   // RESYNC gesture so a re-anchor STICKS
    volatile float sync_slew;      // frames left to shift via a capped rate bend
                                   // (no seek, no dropout); 0 = idle
    volatile bool resync_armed;    // gesture armed (UI)
    float phase_int;
    volatile float phase_err;      // UI display
    float rate_sm;
    volatile float rate;           // UI display
    float out_gain;                // declick ramp

    // waveform thumbnail (reader builds one column per idle pass)
    uint8_t wf[DD_WF_W];
    volatile int wf_state;         // 0 none, 1 building, 2 valid
    int wf_col;

    volatile uint32_t dbg_starve;
} dd_deck_t;

typedef struct {
    dd_deck_t d[2];
    volatile int focus;            // encoder lane focus (0 = A, 1 = B)

    // shared conditioned clock (the whole point of this machine)
    clockin_t ci;
    volatile int clk_src;          // CV channel (default CV8)
    volatile int ppb_idx;          // into dd_ppb[] (mult/div)
    volatile uint32_t pulses;      // accepted pulses since lock (bar phase ref)

    // crossfade: knob6 manual + takeover automation. xf 0 = full A, 1 = full B.
    volatile int xf_cv;            // raw knob (UI display)
    float xf;                      // effective position (smoothed)
    float auto_target;             // takeover fade destination
    volatile bool auto_active;     // an auto-fade is in flight
    float auto_step;               // per-block xf increment while auto-fading
    int   xf_ref;                  // manual-grab latch: knob position at auto
                                   // start; move past DD_XF_GRAB counts = grab
    int   grab_run;                // consecutive moved blocks — a real hand
                                   // move persists, a WiFi ADC spike doesn't
    volatile bool manual;          // knob is live (grabbed / no auto pending)
    volatile int fade_beats;       // Setup: 0 = cut, else 1/4/8 beats

    volatile int loop_len_beats;   // Setup ladder, in QUARTER-beats (dd_loop_q);
                                   // per-deck length freezes at engage
    volatile int layout;           // DD_LAY_V (stacked decks) / DD_LAY_H (panels)

    // CV MATRIX (Arlo: "selectable cv for the functions assignable in each deck.
    // sub menu like the drums"). Every performable function names its own CV
    // channel, and the two loops get their OWN pair per deck — so with the loops
    // on free channels both decks can be worked at once, instead of the trigs'
    // focused-deck compromise. Defaults reproduce the old fixed wiring exactly:
    // filter on CV6, fader on CV7, and both loops BORROWING those same two.
    // The borrow is not special-cased any more: it simply IS what happens when a
    // loop shares a channel with the filter or the fader — the sharing knob goes
    // to the loop while it is engaged and comes back by pass-through pickup.
    // CONTEXTUAL KNOBS ("solo mode", Arlo). Two good knobs, four control sets to
    // reach (filter, fader, and a loop window+length PER DECK) — any FIXED map
    // starves something. So by default CV6/CV7 address the FOCUSED deck, and its
    // LOOP STATUS picks which pair:
    //
    //     focused deck not looping ->  CV6 = filter      CV7 = crossfader
    //     focused deck LOOPING     ->  CV6 = loop window CV7 = loop length
    //
    // Turning the encoder swings the pair to the other deck. The knob whose meaning
    // changes must never step the sound: a knob TAKING OVER a loop param is dead
    // until it MOVES (grab-then-track), and a knob RETURNING to filter/fader is live
    // instantly while the ENGINE catches up to it (never pass-through pickup — that
    // is what left Arlo with a dead crossfader).
    //
    // FADER LOCK is the escape hatch: with both decks looping, no focus position
    // exposes the crossfader. Locked, CV7 stays the fader in every context and the
    // loop LENGTH falls back to its CV Map channel.
    volatile int knob_mode;        // DD_KNOB_CTX / DD_KNOB_FIXED
    volatile bool fader_lock;      // CV7 is ALWAYS the fader
    volatile int cv_fader;         // 0..7
    volatile int cv_filt;
    volatile int cv_lpos[2];       // per deck: loop window position
    volatile int cv_llen[2];       // per deck: loop length

    // master DJ filter (knob7 on the summed mix; deck sweep, fixed q)
    volatile int filt_cv;
    float flt_f;
    volatile int flt_mode;         // 0 off, 1 LP, 2 HP (UI)
    svf_t flt_l, flt_r;
} dd_state_t;

extern dd_state_t dd;
extern const float dd_ppb[6];
extern const char *const dd_ppb_names[6];
// RAW pulses-per-beat: the mult/div setting. This is what the DETECTOR gets
// (clockin_set_ppb) — its sanity gates scale from it, and feeding the octave
// fold back in here creates a relock loop (fold -> gates move -> lock drops ->
// fold resets -> ...; bench-caught on the deck).
#define DD_PPB_RAW() (dd_ppb[dd.ppb_idx])
// EFFECTIVE: the setting x the detector's octave fold. TEMPO MATH uses this —
// mixing the two puts the rate target (and so the beat) in the wrong place.
#define DD_PPB_EFF() (DD_PPB_RAW() * (dd.ci.oct > 0 ? dd.ci.oct : 1.0f))
// loop-length ladder in QUARTER-beats (the deck's, verbatim): 1/4 .. 256 beats
#define DD_LOOP_STEPS 11
extern const int dd_loop_q[DD_LOOP_STEPS];
void dd_fmt_beats(int q, char *out, int n);   // 1 -> "1/4", 2 -> "1/2", 4 -> "1"

// UI-side (SD-touching; UI/background tasks only)
int  dualdeck_load_track(int deck, const char *name);
// audio-task-safe transport arms (engine fires them on the bar)
void dualdeck_arm_start(int deck);
void dualdeck_arm_stop(int deck);
void dualdeck_loop_toggle(int deck);   // TR2 grammar; safe from UI too
void dualdeck_resync(int deck);        // BOTH-TRIG gesture: the beat lands NOW
void dualdeck_rearm_loop_knobs(int deck);   // re-target a loop CV: dead until MOVED
