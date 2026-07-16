#pragma once
#include <stdint.h>
#include <stdbool.h>

// M3 slicer — STREAMING edition (2026-07-13, "the real fix, deck-sized").
// The whole-sample PSRAM buffer is gone and with it the length ceiling: any
// pool sample (RAW/WAV/AIFF via sampfile) of any length slices now.
//
//   attack:  every slice keeps an 80 ms HEAD in one PSRAM slab, stored in
//            PLAYBACK ORDER (reverse heads are pre-flipped at build time),
//            so a trigger is instant and the voice never knows the direction
//   tail:    a reader task streams the playing slice's remainder into a 2 s
//            ring (deck discipline: sd_lock per burst, request flags, the
//            reader is the only file toucher; process() reads PSRAM only)
//
// Trigger latency = head (RAM). Tail arrival budget: at 2.0x pitch an 80 ms
// head lasts 40 ms wall; the reader's FIRST chunk is small (1024 frames) so
// the ring goes live in ~15 ms. Retrigger = new generation; a stale ring is
// simply ignored until the reader catches up (the head covers the gap).

#define SL_RATE        44100
#define SL_HEAD_FRAMES 3528                  // 80 ms per-slice attack head
#define SL_MAX_SLICES  128
#define SL_RING_FRAMES (SL_RATE * 2)         // 2 s tail ring (~352 KB)
#define SL_WIN         512                   // transient envelope hop
#define SL_XFADE       64                    // ~1.45 ms fire crossfade (declick on interrupting fire)
#define SL_ENV_MAX     (10 * 60 * SL_RATE / SL_WIN + 2)   // <=10 min detection
#define SL_PEAKS       300                   // waveform display columns
#define SL_OT_SLICES   64                    // Elektron .ot format limit

typedef struct {
    // PSRAM (allocated once at start; each slab under the ~2.1 MB grant ceiling)
    int16_t *heads;               // SL_MAX_SLICES * SL_HEAD_FRAMES stereo frames
    int16_t *ring;                // SL_RING_FRAMES stereo frames (tail)
    float   *env;                 // transient envelope (reader-built per load)
    volatile uint32_t env_n;      // envelope windows valid

    uint32_t head_len[SL_MAX_SLICES];  // frames valid per head (reader writes
                                       // under the loading gate)
    volatile bool heads_valid;

    volatile uint32_t len;        // file frames (sampfile probe)
    volatile bool loading;        // load/reslice in progress — engine silent
    char sample[24];              // loaded sample id (no extension)

    // reader request protocol (UI/audio set flags; READER acts)
    volatile bool load_req;
    char pending[24];
    volatile bool resl_req;       // recompute boundaries + rebuild heads

    // stream generations: engine bumps gen on every fire; the reader fills
    // the ring for that generation and publishes ring_gen/ring_avail. The
    // voice touches the ring only when ring_gen matches its own generation.
    volatile uint32_t gen;
    volatile int      gen_slice;
    volatile uint32_t ring_gen;
    volatile uint32_t ring_avail; // playback-order frames available (slice-rel)
    volatile uint32_t vpos_i;     // voice position (slice-rel), for reader lead

    // settings (UI writes, engine reads)
    volatile int  slice_target;
    volatile int  n_slices;
    volatile bool transient_mode;
    volatile int  sensitivity;
    uint32_t slice_pt[SL_MAX_SLICES + 1];
    volatile int  sel;
    volatile bool auto_on;
    volatile bool reverse;        // direction (reader rebuilds heads on change)
    volatile uint16_t level;
    volatile uint16_t pitch_cv;

    // playback voice (audio task)
    volatile bool playing;
    volatile int  cur;
    double  pos;                  // slice-relative playback-order position
    uint32_t s_len;               // current slice length (frames)
    float   inc;
    volatile uint32_t dbg_starve; // blocks the tail wasn't ready
    float   last_l, last_r;       // last output frame (declick / starve decay)
    float   xf_l, xf_r;           // crossfade-FROM (last output at the last fire)
    int     xfade;                // frames left in the fire crossfade (0 = none)

    // Octatrack .ot sidecar
    uint32_t ot_pt[SL_OT_SLICES + 1];
    int      ot_n;
    volatile bool ot_present;
    volatile bool ot_active;

    // one-shot commands (engine consumes)
    volatile uint8_t cmd_fire;
    volatile uint8_t cmd_advance;

    // waveform display (reader-built per load)
    volatile int  peak_n;
    uint8_t peaks[SL_PEAKS];
} sl_state_t;

extern sl_state_t sl;

// request a (re)load — ASYNC now: the reader opens/probes/scans (UI task safe)
int slicer_load(const char *name);
// request boundary recompute + head rebuild (ASYNC; brief mute while it runs)
void slicer_reslice(void);
int slicer_list_samples(char out[][24], int max);

// Octatrack .ot sidecar I/O (slicer_ot.c)
int slicer_parse_ot(const char *name, uint32_t sample_len, uint32_t *out_pt, int max_pts);
int slicer_build_ot(uint8_t out[832], float bpm);
