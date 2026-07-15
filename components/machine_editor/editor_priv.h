#pragma once
#include <stdint.h>
#include <stdbool.h>

// Audio editor — offline, NON-DESTRUCTIVE file->file ops on pool samples: each
// op streams the source (sampfile) through a transform and writes a NEW derived
// take into the pool (usr/<src>_<tag>.WAV) via sampwav. Runs on a background
// task under sd_lock (the import/analysis discipline); process() is silent.
// v1 ops: normalize (2-pass peak), reverse, fade in, fade out, trim silence.

#define ED_RATE  44100
#define ED_CHUNK 2048          // frames per read/write burst (8 KB int16 stereo)
#define ED_NAME_LEN 24

enum { ED_IDLE = 0, ED_RUNNING, ED_DONE, ED_ERR };
enum { OP_NORMALIZE = 0, OP_REVERSE, OP_FADEIN, OP_FADEOUT, OP_TRIM, OP_N };

typedef struct {
    volatile int  state;                 // ED_*
    volatile int  op;                    // OP_*
    volatile int  progress;              // 0..100
    char src[ED_NAME_LEN];               // source id
    char out[ED_NAME_LEN];               // produced id
    char err[48];
    float param;                         // fade ms (fades) / threshold (trim), 0 = default
} ed_state_t;

extern ed_state_t ed;
extern const char *const ed_op_names[OP_N];

// kick an op (from the web handler / menu). No-op if a job is already running.
void editor_apply(const char *src, int op, float param);
