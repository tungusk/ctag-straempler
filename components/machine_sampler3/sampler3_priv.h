#pragma once
#include <stdint.h>
#include <stdbool.h>

// Sampler3 — the two-voice sampler rebuilt on the deck/tracker architecture:
// one unpinned READER task owns ALL file I/O and fills per-voice PSRAM
// buffers; process() only consumes them and flips request flags the reader
// acts on. No SD, no heap, no locks, no cJSON in the audio task — ever.
//
// Streaming model (per voice): playback-ORDER frame space. Frame p is the
// p'th frame the listener hears, so the engine is direction-agnostic:
//   forward:  file frame = start + p
//   reverse:  file frame = (start + len - 1) - p
// The reader does the mapping (and reverses chunks for reverse mode).
//   head[]  caches playback-order frames [0 .. head_frames)  — loaded once
//           per assign/trim/direction change; makes retrigger + loop wrap
//           INSTANT (no SD round-trip on a gate).
//   ring[]  a sliding window of frames [head_frames .. wpos); slot = p %
//           S3_RING_FRAMES. Samples short enough to fit head+ring are
//           effectively RAM-resident: zero SD traffic after load.
// process() for frame p: p < head_frames -> head; p+1 < wpos -> ring;
// else decay-mute (starve counted, never reads past wpos).

#define S3_RATE        44100
#define S3_HEAD_FRAMES (S3_RATE * 1)      // 1 s stereo head (~176 KB PSRAM)
#define S3_RING_FRAMES (S3_RATE * 4)      // 4 s stereo ring (~706 KB PSRAM)
#define S3_NAME_LEN    24
#define S3_NVOICES     2

enum { S3_MODE_ONESHOT = 0, S3_MODE_LOOP };
// per-voice pitch source, Settings-switchable. OFF = fixed 1.0 (the 1V/oct
// jacks idle at ~21%, which the keyboard map would read as -1 octave)
enum { S3_PITCH_OFF = 0, S3_PITCH_1VOCT, S3_PITCH_SPEED };

typedef struct {
    // -- assignment (reader-owned once load_req is raised) ------------------
    char name[S3_NAME_LEN];          // loaded sample id ("" = unloaded)
    volatile uint32_t file_frames;   // whole file length (frames)
    // -- playback window (UI writes; reader snapshots into play_* on reload) --
    volatile float start_pct;        // trim window start 0..1
    volatile float len_pct;          // trim window length 0..1 (of remainder)
    volatile bool  reverse;
    // reader's applied window (playback-order space is built from these)
    volatile uint32_t play_start;    // file frame of window start
    volatile uint32_t play_len;      // window length in frames
    // -- buffers + cursors (deck protocol: reader owns wpos + seek-writes of
    //    rpos; audio task owns rpos during playback; volatile is enough for
    //    aligned 32-bit cursors on this core pair) --------------------------
    int16_t *head;                   // PSRAM, S3_HEAD_FRAMES * 2
    volatile uint32_t head_frames;   // valid playback-order frames in head
    volatile bool head_valid;        // false while (re)building the head
    int16_t *ring;                   // PSRAM, S3_RING_FRAMES * 2
    volatile uint32_t wpos;          // stream write head (playback-order, absolute)
    volatile uint32_t rpos_i;        // play cursor (playback-order, absolute)
    double   rpos_f;                 // fractional part (engine only)
    // -- request flags: UI/audio tasks ONLY set these; the reader clears and
    //    applies them (including any cursor rewrites) -----------------------
    volatile bool load_req;          // assign `pending` to this voice
    char pending[S3_NAME_LEN];
    volatile bool window_req;        // trim/reverse changed: rebuild head+stream
    volatile bool retrig_req;        // gate: restart stream fill at head end
    // -- transport (audio-task-owned; loading is set by requesters and
    //    cleared by the reader, deck protocol) ------------------------------
    volatile bool playing;
    volatile bool loading;           // stream (re)filling; ring reads parked
    volatile int  playmode;          // S3_MODE_*
    // -- params (UI-owned, audio reads) --------------------------------------
    volatile int   pitch_mode;       // S3_PITCH_*
    volatile float level;            // 0..1
    volatile float pan;              // -1..1
    // -- engine-local ---------------------------------------------------------
    float out_gain;                  // declick ramp
    float last_l, last_r;            // decay-mute tail
    int   cv_floor;                  // floor tracker for the 1V/oct jack
    // -- diagnostics ----------------------------------------------------------
    volatile uint32_t dbg_starve;    // blocks starved mid-play (reader behind)
} s3_voice_t;

typedef struct {
    s3_voice_t v[S3_NVOICES];
    volatile bool monitor;           // pass line-in through while armed+stopped
    // recording UI state (mirrors the core recording service)
    volatile int  arm_target;        // -1 none, 0/1 = voice armed for recording
    volatile bool save_failed;       // last take failed to save (UI banner)
    char last_rec[S3_NAME_LEN];      // last auto-picked recording (UI)
} s3_state_t;

extern s3_state_t s3;

// UI-side (SD-touching or flag-setting; call from UI/menu tasks only)
void s3_load_sample(int vid, const char *name);  // browse-assign (sets load_req)
void s3_set_window(int vid, float start_pct, float len_pct, bool reverse);
void s3_toggle_arm(int vid);                     // explicit arm from the Record page
int  s3_list_samples(char (**names)[S3_NAME_LEN]); // usr/*.RAW browser list
