// Tape — a single-track tape recorder/editor with eyes. One mono PSRAM tape
// (15/30/60 s): record line-in onto it, or load any pool sample; the screen
// is ONE BIG WAVEFORM with a crop window, playhead and a beat grid anchored
// at the IN point (grid from the shared clock / beat listener, else a manual
// BPM). Playback loops the crop; the FX chain (SVF filter -> drive -> reverb)
// sits IN the signal path, so incoming audio monitors through it and the
// record head taps POST-FX (you print the effects; Rec Source = TAPE
// re-prints the tape through the chain). Crop ops: cut/copy/paste
// (PSRAM clipboard), normalize, reverse, fade edges, save -> pool take.
// Destructive ops require STOP (the audio task never sees a moving buffer).
#ifndef TAPE_PRIV_H
#define TAPE_PRIV_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>      // tp_cut_eff's powf
#include "clock.h"
#include "svf.h"
#include "reverb.h"
#include "fxdelay.h"
#include "overdrive.h"
#include "flanger.h"
#include "tremolo.h"
#include "fxfilter.h"
#include "fxrack.h"
#include "cvmtx.h"

#define TP_RATE      44100
#define TP_PEAKS     300              // overview columns
#define TP_FADE_MS   15               // "Fade Edges" ramp
#define TP_LEN_OPTS  3                // 15 / 30 / 60 s

// The tape is a BLOCK LIST, not one slab: single PSRAM allocs > ~2.1 MB are
// refused on this board (slicer lesson — and exactly how tape-v1's first
// boot failed: the 30 s tape is a 2.65 MB alloc). The clipboard uses the same
// shape.
//
// BANKS ARE 128 KB (2^16 frames), down from 1 MiB (2026-07-25). PSRAM on this
// board totals 4.00 MB — the ESP32 can only map 4 MB of external RAM into its
// address space, so that is a HARDWARE ceiling, not a config. At 1 MiB
// granularity a 30 s tape (2.52 MB of audio) rounded up to 3 whole banks and
// threw away 0.48 MB — a sixth of the entire pool — for nothing. Finer banks
// hand that back to the tape, which is what buys the extra seconds.
#define TP_BLK_SHIFT  16
#define TP_BLK_FRAMES (1u << TP_BLK_SHIFT)
#define TP_BLK_MASK   (TP_BLK_FRAMES - 1u)
#define TP_MAX_BLK    36              // ceiling is free PSRAM, not this (36 = 52 s)

typedef struct {
    int16_t *blk[TP_MAX_BLK];
    int      nblk;
    uint32_t cap;                     // frames
} tp_bank_t;

static inline int16_t bank_rd(const tp_bank_t *b, uint32_t i)
{
    return b->blk[i >> TP_BLK_SHIFT][i & TP_BLK_MASK];
}
static inline void bank_wr(tp_bank_t *b, uint32_t i, int16_t v)
{
    b->blk[i >> TP_BLK_SHIFT][i & TP_BLK_MASK] = v;
}

enum { TPF_OFF = 0, TPF_LP, TPF_BP, TPF_HP, TPF_N };
enum { TPS_INPUT = 0, TPS_TAPE };     // record source
enum { TPR_PUNCH = 0, TPR_MOMENTARY }; // TR2 record behaviour: edge-toggle vs gate-held
enum { TPD_TAPE = 0, TPD_CARD };      // record destination: PSRAM loop tape / long WAV to card
enum { TPQ_OFF = 0, TPQ_BEAT, TPQ_BAR }; // punch-out quantize: immediate / next beat / next bar

typedef struct {
    // tape
    tp_bank_t tape;                   // PSRAM banks, mono
    uint32_t cap;                     // usable frames (== tape.cap)
    uint32_t len;                     // used extent (record/load high-water)
    int      len_sel;                 // 0/1/2 -> 15/30/60 s (realloc on change)
    // crop window (frames; out exclusive; in < out <= len when len > 0)
    uint32_t in_pt, out_pt;
    // transport
    double   pos;                     // playhead
    volatile bool playing;
    volatile bool recording;          // record state (driven by TR2)
    volatile bool rec_extend;         // this record pass started from empty -> extend len (fill)
    int      rec_mode;                // TPR_PUNCH | TPR_MOMENTARY (Setup)
    int      rec_dest;                // TPD_TAPE | TPD_CARD (Setup): loop tape vs long WAV
    int      rec_quant;              // TPQ_* (Setup): quantize punch-out to beat/bar
    uint32_t rec_stop_target;        // frame to finalize a fresh take at (0 = none pending)
    uint32_t tr2_hold;                // frames TR2 gate held (punch: long-hold = erase, arm)
    bool     tr2_armed;               // long-hold erased the tape -> record starts on release
    bool     tr2_recgest;             // this TR2 press STARTED a recording (overdub or fresh take)
                                      // -> holding it converts the take into an erase + re-arm.
                                      // Not set by a punch-OUT press, so holding after punching
                                      // out can never wipe the take you just finished.
    int      rec_src;                 // TPS_INPUT | TPS_TAPE (re-print)
    bool     monitor;                 // hear input through FX while stopped
    int      fx_route;                // TPFX_* — where the chain sits vs the record head
    bool     play_oneshot;            // crop end: stop (one-shot) vs wrap (loop, default)
    // grid
    clockin_t ci;
    int      clk_src;                 // CV1..8 / AUDIO (clock.h encoding)
    float    manual_bpm;              // used when the clock is unlocked
    // Setup filter + drive: a per-sample svf + cubic soft-clip on the INCOMING
    // audio, AHEAD of the FX rack (printed to tape). Distinct from the rack's
    // FILT/BAND bricks — this is Tape's own always-there tone stage.
    int      flt_mode;                // TPF_*
    float    cutoff;                  // Hz
    float    res01;
    float    drive;                   // 0..1 cubic soft-clip amount
    svf_t    flt;
    // FX rack: the shared curated slot rack (FX1/FX2 generic + FX3 reverb). Tape
    // owns these effect instances; tp_rk is a pointer-view over them. Rate effects
    // are clock-synced (Tape sets tp_rk.bpm each block -> dub delays lock to grid).
    reverb_t    rv;
    fxdelay_t   dly;
    overdrive_t od;
    flanger_t   flg;
    tremolo_t   trem;
    fxfilter_t  filt;                 // rack LP/HP/BP brick
    fxfilter_t  band;                 // rack base/width brick
    int8_t      fx_slot[FX_NSLOT_GEN];
    float    level;                   // output volume (post-record-tap)
    // CV matrix (the shared cvmtx widget, Tape = first host): live OFFSETS,
    // non-destructive like win_move. IN/OUT move their crop points and WIN
    // slides the whole window (each as amt * CV, in fractions of len);
    // LEVEL adds onto the output master (full-negative = VCA-style duck to
    // silence); CUTOFF offsets the Setup filter in the LOG domain (matches
    // K6's 30*200^x taper). Values are computed once per block in the audio
    // task into mx_* and read wherever the base value applies — including
    // tape_eff_window, which the UI task also calls (aligned float
    // load/store is atomic on this core, the win_move precedent).
    cvmtx_t  mtx;
    volatile float mx_in, mx_out, mx_win;   // -1..+1, fraction of len
    volatile float mx_lvl;                  // -1..+1, additive on level
    volatile float mx_cut;                  // -1..+1, log-domain octaves-ish
    // knobs 5..8 with takeover: win move / cutoff / res / drive
    float    knob_capt[4];
    bool     knob_live[4];
    int      knob_ctx;
    float    win_move;                // K5: -1..1 window shift (0 at noon)
    // clipboard
    tp_bank_t clip;                   // PSRAM banks, lazy
    uint32_t clip_len;
    // overview peaks (rebuilt by the UI task; dirty range for live record)
    uint8_t  peaks[TP_PEAKS];
    volatile uint32_t peaks_done;     // frames covered by peaks so far
    // background save job (Save Crop + auto-save both route through it)
    volatile bool save_busy;
    char     save_id[12];             // last minted take id ("" = none)
    uint32_t save_a, save_b;          // frames [a,b) the writer will emit
    bool     save_crop;              // true = an actively-cropped take (marked TCR_)
    // The writer reads the bank in the background while the audio task may still
    // be recording INTO it (an overdub punch during a drop, or a fresh take
    // rolling over a deferred auto-save). The audio task sets this when it writes
    // a frame inside [save_a,save_b), so the file is reported as suspect rather
    // than quietly containing a blend of the old and new material.
    volatile bool drop_spoiled;
    // crop-drop ADOPTION: after the writer closes the file, the UI task loads it
    // back as the tape's content (the "crop" half of the gesture). Deferred
    // because the load must not race the writer, and it needs UI context (SD).
    char     adopt_id[12];           // "" = nothing pending
    bool     adopt_resume;           // transport was rolling -> play the new take
    // monitor mute while a load streams from the card: without it you sit on the
    // raw line-in for the whole read (very audible on the crop's load-back, which
    // happens mid-performance). Slewed, so muting doesn't click.
    volatile bool loading;           // a tape_load() is in flight
    float    mute_g;                 // 0..1 smoothed monitor gain
    // auto-save: takes persist to the card when you move on from them (a fresh
    // take overwrites, or you leave Tape). A recorded buffer is never lost.
    bool     take_dirty;             // buffer holds unsaved recorded audio
    bool     cropped;                // user actively set the crop on this take
    volatile bool autosave_req;      // audio task -> UI task: spawn the deferred save
    volatile bool pending_fresh;     // fresh take queued, waiting for the save to finish
    // session continuity: the id of what's in the buffer (last saved take id or
    // loaded sample name); persisted to CONFIG "tapelast" on leave and reloaded
    // on return so work-in-progress survives a machine switch.
    char     restore_id[24];
    bool     restore_pending;        // reload the persisted take on first Tape-screen entry
    int      take_num;               // session take counter -> "REC-###" title until saved
    // load progress ("" = idle) for the menu
    char     load_note[24];
    // Crop-drop confirmation for the Live page: the drop is a background copy
    // with no visible effect on the tape, so without this it looks like nothing
    // happened. Holds the dropped id; counts down on the SLOW menu tick.
    char     drop_note[12];
    int      drop_ticks;
    // ui hints
    float    disp_bpm;                // effective grid bpm for the header
    bool     disp_clk;                // true = grid comes from the clock
} tape_state_t;

extern tape_state_t tp;
extern fxrack_t     tp_rk;        // pointer-view over tp's effect instances

// CV matrix destination order (tp.mtx rows; labels in tape.c). The FX rows
// modulate the rack's curated per-slot param pair (fxrack_t.cv1/cv2 — what
// A/B mean follows the loaded effect; the row label tracks it) + reverb mix.
enum { TPM_IN = 0, TPM_OUT, TPM_WIN, TPM_LVL, TPM_CUT,
       TPM_FX1A, TPM_FX1B, TPM_FX2A, TPM_FX2B, TPM_RVMX, TPM_N };

// labels are LIVE (the FX rows rename with the slot's effect): tape.c owns the
// array; the menu refreshes it on CV-page entry
extern const char *tape_mtx_labels[TPM_N];
void tape_mtx_refresh_labels(void);

static inline float tp_clampf(float x, float lo, float hi){ return x < lo ? lo : x > hi ? hi : x; }
static inline int   tp_clampi(int x, int lo, int hi){ return x < lo ? lo : x > hi ? hi : x; }
static inline int16_t tp_rd(uint32_t i){ return bank_rd(&tp.tape, i); }
static inline void    tp_wr(uint32_t i, int16_t v){ bank_wr(&tp.tape, i, v); }

// FX ROUTE — where the chain sits relative to the RECORD HEAD:
//   PRE   chain colours incoming audio and is PRINTED to tape; playback dry
//         (the original Tape behaviour, and what old presets load as)
//   POST  tape records DRY, chain colours the OUTPUT including playback — audition
//         and change effects over a take without committing them
//   OFF   chain bypassed entirely — dry in, dry out, nothing printed
//
// POST LATCHES: the first punch-in flips the route to PRE and leaves it there,
// so you audition effects over a dry take live and the moment you commit, what
// you were hearing starts being printed. (This began as a separate AUTO mode;
// Arlo 2026-07-25: "maybe there doesn't need to be an auto, that can just be
// the post behavior".) Set it back by hand to audition again.
enum { TPFX_PRE = 0, TPFX_POST, TPFX_OFF, TPFX_N };
static const char *const TPFX_NAMES[TPFX_N] = { "pre", "post", "off" };
// the route resolved for THIS block — the one thing the engine actually asks
static inline bool tp_fx_is_post(void){ return tp.fx_route == TPFX_POST; }

// One mute step per output frame (~2.3 ms time constant, so a fade rather than
// a click). Call EXACTLY once per frame, in the final output loop.
#define TP_MUTE_SLEW 0.01f
static inline float tp_mute_step(void){
    float t = tp.loading ? 0.0f : 1.0f;
    tp.mute_g += (t - tp.mute_g) * TP_MUTE_SLEW;
    return tp.mute_g;
}

// effective (matrix-offset) output level + filter cutoff — every consumer of
// tp.level / tp.cutoff in the signal path goes through these
static inline float tp_lvl_eff(void){ return tp_clampf(tp.level + tp.mx_lvl, 0.0f, 1.2f); }
static inline float tp_cut_eff(void){
    float c = tp.cutoff;
    if (tp.mx_cut != 0.0f) {
        // squared taper = fine control near zero; full amount sweeps the
        // whole K6 log range (x200 / /200)
        float m = tp.mx_cut * tp.mx_cut * (tp.mx_cut < 0 ? -1.0f : 1.0f);
        c *= powf(200.0f, m);
    }
    return tp_clampf(c, 20.0f, 6500.0f);
}

// effective crop window after the K5 window-move performance offset
void tape_eff_window(uint32_t *in, uint32_t *out);
// grid beat length in frames (clock if locked, else manual bpm)
uint32_t tape_beat_frames(void);

// ---- UI-context operations -------------------------------------------------
// All of these MUTATE the buffer and so refuse unless the transport is stopped —
// except tape_save_crop, which only reads (see its note below).
int  tape_set_len_sel(int sel);           // realloc tape (clears it). 0 ok
int  tape_load(const char *name);         // pool sample -> tape. 0 ok
void tape_clear(void);
void tape_norm(void);                     // normalize crop to -0.5 dBFS
void tape_reverse(void);                  // reverse crop
void tape_fade(void);                     // TP_FADE_MS ramps at crop edges
int  tape_copy(void);                     // crop -> clipboard. 0 ok
int  tape_cut(void);                      // copy + delete crop (close gap)
int  tape_paste(void);                    // insert clipboard at IN. 0 ok
// CROP: write the audible loop to usr/TAPE/TCR_NNNN.WAV, then load that file
// back as the tape's content. Legal WHILE PLAYING (refuses only while
// recording). Nothing is lost — an unsaved take is persisted whole before the
// buffer is replaced. 0 = the writer started.
int  tape_save_crop(void);
void tape_drop_adopt_kick(void);          // UI task: finish the crop once the writer closes
void tape_crop_beats(int beats);          // out = in + beats * beat
uint32_t tape_snap(uint32_t frame);       // grid beat (bpm known) else zero-cross
void tape_rebuild_peaks(bool full);       // UI task; incremental while recording
void tape_autosave_kick(void);            // UI task: spawn the deferred auto-save if requested
void tape_restore_last(void);             // UI task: reload the persisted take (session continuity)
void tape_card_service(void);             // UI task: re-arm the streaming recorder in card mode

#endif
