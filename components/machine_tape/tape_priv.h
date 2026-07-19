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
#include "clock.h"
#include "svf.h"
#include "reverb.h"
#include "fxdelay.h"
#include "overdrive.h"
#include "flanger.h"
#include "tremolo.h"
#include "fxfilter.h"
#include "fxrack.h"

#define TP_RATE      44100
#define TP_PEAKS     300              // overview columns
#define TP_FADE_MS   15               // "Fade Edges" ramp
#define TP_LEN_OPTS  3                // 15 / 30 / 60 s

// The tape is a BLOCK LIST, not one slab: single PSRAM allocs > ~2.1 MB are
// refused on this board (slicer lesson — and exactly how tape-v1's first
// boot failed: the 30 s tape is a 2.65 MB alloc). 1 MiB banks (2^19 frames);
// the clipboard uses the same shape.
#define TP_BLK_SHIFT  19
#define TP_BLK_FRAMES (1u << TP_BLK_SHIFT)
#define TP_BLK_MASK   (TP_BLK_FRAMES - 1u)
#define TP_MAX_BLK    6               // 60 s = 2.646 M frames -> 6 banks

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
    bool     tr2_overdub;             // this TR2 press started an overdub (convertible to erase)
    int      rec_src;                 // TPS_INPUT | TPS_TAPE (re-print)
    bool     monitor;                 // hear input through FX while stopped
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
    // ui hints
    float    disp_bpm;                // effective grid bpm for the header
    bool     disp_clk;                // true = grid comes from the clock
} tape_state_t;

extern tape_state_t tp;
extern fxrack_t     tp_rk;        // pointer-view over tp's effect instances

static inline float tp_clampf(float x, float lo, float hi){ return x < lo ? lo : x > hi ? hi : x; }
static inline int   tp_clampi(int x, int lo, int hi){ return x < lo ? lo : x > hi ? hi : x; }
static inline int16_t tp_rd(uint32_t i){ return bank_rd(&tp.tape, i); }
static inline void    tp_wr(uint32_t i, int16_t v){ bank_wr(&tp.tape, i, v); }

// effective crop window after the K5 window-move performance offset
void tape_eff_window(uint32_t *in, uint32_t *out);
// grid beat length in frames (clock if locked, else manual bpm)
uint32_t tape_beat_frames(void);

// ---- UI-context operations (all refuse while playing/recording) ------------
int  tape_set_len_sel(int sel);           // realloc tape (clears it). 0 ok
int  tape_load(const char *name);         // pool sample -> tape. 0 ok
void tape_clear(void);
void tape_norm(void);                     // normalize crop to -0.5 dBFS
void tape_reverse(void);                  // reverse crop
void tape_fade(void);                     // TP_FADE_MS ramps at crop edges
int  tape_copy(void);                     // crop -> clipboard. 0 ok
int  tape_cut(void);                      // copy + delete crop (close gap)
int  tape_paste(void);                    // insert clipboard at IN. 0 ok
int  tape_save_crop(void);                // background: crop -> usr/TCR_NNNN.WAV (marked)
void tape_crop_beats(int beats);          // out = in + beats * beat
uint32_t tape_snap(uint32_t frame);       // grid beat (bpm known) else zero-cross
void tape_rebuild_peaks(bool full);       // UI task; incremental while recording
void tape_autosave_kick(void);            // UI task: spawn the deferred auto-save if requested
void tape_restore_last(void);             // UI task: reload the persisted take (session continuity)
void tape_card_service(void);             // UI task: re-arm the streaming recorder in card mode

#endif
