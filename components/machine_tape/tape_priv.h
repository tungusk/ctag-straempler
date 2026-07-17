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

#define TP_RATE      44100
#define TP_PEAKS     300              // overview columns
#define TP_FADE_MS   15               // "Fade Edges" ramp
#define TP_LEN_OPTS  3                // 15 / 30 / 60 s

enum { TPF_OFF = 0, TPF_LP, TPF_BP, TPF_HP, TPF_N };
enum { TPS_INPUT = 0, TPS_TAPE };     // record source

typedef struct {
    // tape
    int16_t *buf;                     // PSRAM, cap frames, mono
    uint32_t cap;                     // allocated frames
    uint32_t len;                     // used extent (record/load high-water)
    int      len_sel;                 // 0/1/2 -> 15/30/60 s (realloc on change)
    // crop window (frames; out exclusive; in < out <= len when len > 0)
    uint32_t in_pt, out_pt;
    // transport
    double   pos;                     // playhead
    volatile bool playing;
    volatile bool recording;          // punch state (TR2 / UI)
    int      rec_src;                 // TPS_INPUT | TPS_TAPE (re-print)
    bool     monitor;                 // hear input through FX while stopped
    // grid
    clockin_t ci;
    int      clk_src;                 // CV1..8 / AUDIO (clock.h encoding)
    float    manual_bpm;              // used when the clock is unlocked
    // fx chain (in the path: source -> filter -> drive -> reverb -> out+tape)
    int      flt_mode;                // TPF_*
    float    cutoff;                  // Hz
    float    res01;
    float    drive;                   // 0..1 cubic soft-clip amount
    reverb_t rv;
    svf_t    flt;
    float    level;                   // output volume (post-record-tap)
    // knobs 5..8 with takeover: win move / cutoff / res / drive
    float    knob_capt[4];
    bool     knob_live[4];
    int      knob_ctx;
    float    win_move;                // K5: -1..1 window shift (0 at noon)
    // clipboard
    int16_t *clip;                    // PSRAM, lazy
    uint32_t clip_len;
    // overview peaks (rebuilt by the UI task; dirty range for live record)
    uint8_t  peaks[TP_PEAKS];
    volatile uint32_t peaks_done;     // frames covered by peaks so far
    // save-crop job
    volatile bool save_busy;
    char     save_id[12];             // last minted take id ("" = none)
    // ui hints
    float    disp_bpm;                // effective grid bpm for the header
    bool     disp_clk;                // true = grid comes from the clock
} tape_state_t;

extern tape_state_t tp;

static inline float tp_clampf(float x, float lo, float hi){ return x < lo ? lo : x > hi ? hi : x; }
static inline int   tp_clampi(int x, int lo, int hi){ return x < lo ? lo : x > hi ? hi : x; }

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
int  tape_save_crop(void);                // background: crop -> usr/TAP_NNNN.WAV
void tape_crop_beats(int beats);          // out = in + beats * beat
uint32_t tape_snap(uint32_t frame);       // grid beat (bpm known) else zero-cross
void tape_rebuild_peaks(bool full);       // UI task; incremental while recording

#endif
