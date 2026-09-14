#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sample_ram.h"   // SAMPLE_ID_LEN

// Audio editor — offline, NON-DESTRUCTIVE file->file ops on pool samples: each
// op streams the source (sampfile) through a transform and writes a NEW derived
// take into the pool (usr/<src>_<tag>.WAV) via sampwav. Runs on a background
// task under sd_lock (the import/analysis discipline); process() is silent.
// v1 ops: normalize (2-pass peak), reverse, fade in, fade out, trim silence.

#define ED_RATE  44100
#define ED_CHUNK 2048          // frames per read/write burst (8 KB int16 stereo)
#define ED_NAME_LEN SAMPLE_ID_LEN  // was 24; see SAMPLE_ID_LEN (sample_ram.h) — one definition for pool ids
#define ED_PEAKS 300           // waveform columns, same budget as Tape/Slicer

enum { ED_IDLE = 0, ED_RUNNING, ED_DONE, ED_ERR };
// OP_CROP is appended, NOT inserted: /edit/apply takes the op as an INDEX and
// the web card builds its buttons from this order, so renumbering would make
// old links do the wrong thing.
enum { OP_NORMALIZE = 0, OP_REVERSE, OP_FADEIN, OP_FADEOUT, OP_TRIM, OP_CROP, OP_N };

typedef struct {
    volatile int  state;                 // ED_*
    volatile int  op;                    // OP_*
    volatile int  progress;              // 0..100
    char src[ED_NAME_LEN];               // source id
    char out[ED_NAME_LEN];               // produced id
    char err[48];
    float param;                         // fade ms (fades) / threshold (trim), 0 = default

    // Edit RANGE (2026-09-12). Ops transform only [in, out) and pass the rest
    // of the file through unchanged; OP_CROP writes the range and nothing else.
    // in_pt = 0, out_pt = frames reproduces the original whole-file behaviour
    // exactly, which is what the web path sends when it omits them. Named after
    // Tape's crop points, because that is the same idea and will be the same UI.
    uint32_t in_pt, out_pt;
    uint32_t frames;                     // frames in the loaded source, 0 = none

    // Waveform columns for the panel strip, binned over the WHOLE file (that is
    // what wave_edit expects). Built by a background scan, one chunked pass,
    // the same discipline the ops use.
    uint8_t  peaks[ED_PEAKS];
    volatile bool scanning;
    volatile int  scan_pct;

    // Clipboard. Tape's lives in a PSRAM bank, which caps it at the tape length;
    // the Editor's is a FILE so cut/copy/paste inherit the any-length property
    // the streaming engine already has.
    volatile bool clip_full;
    uint32_t clip_frames;
} ed_state_t;

extern ed_state_t ed;
extern const char *const ed_op_names[OP_N];

// kick an op (from the web handler / menu). No-op if a job is already running.
// in/out are frame bounds; out == 0 means "to the end of the file".
void editor_apply(const char *src, int op, float param, uint32_t in, uint32_t out);

// Probe a pool sample's length without loading it (panel Source row).
// Returns frames, 0 on failure.
uint32_t editor_probe(const char *name);

// Load a pool sample as the edit source: probes it, resets the range to the
// whole file, and kicks the background peak scan. 0 ok.
int  editor_load(const char *name);

// ---- audition --------------------------------------------------------------
// The crop window loops while you drag it, which is the whole reason to have a
// waveform: an edit point set by eye alone is a guess.
void editor_audition(bool on);
bool editor_auditioning(void);
void editor_audition_window(void);      // re-arm the loop after in/out moved
uint32_t editor_play_pos(void);

// ---- clipboard (streaming, file-backed) ------------------------------------
int  editor_copy(void);                 // [in,out) -> clipboard
int  editor_cut(void);                  // clipboard, and a new take without the range
int  editor_paste(void);                // a new take with the clipboard inserted at in_pt

// ---- slice -----------------------------------------------------------------
// Writes N takes into usr/SLICES plus an .OT sidecar, so the Slicer machine can
// load the map directly. 0 ok.
int  editor_slice(int n);
