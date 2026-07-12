#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "clock.h"

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
#define S3_WF_W        144                // waveform thumbnail columns

enum { S3_MODE_ONESHOT = 0, S3_MODE_LOOP };
// crop behavior: OFF bypasses the window entirely; FREE = continuous
// points; QUANT snaps start+length to whole beats of the take's stamped
// tempo; QUANT2 = musical ladder — length picks from 1/2/4/8/16/32 beats
// and start snaps to the phrase grid of that length (a 4-beat window sits
// on 4-beat boundaries, KO II loop feel). Both fall back to FREE behavior
// when the sample has no bpm.
enum { S3_CROP_OFF = 0, S3_CROP_FREE, S3_CROP_QUANT, S3_CROP_QUANT2 };
// CV matrix: each destination below carries its own source assignment
// (-1 = off, 0..7 = CV1..CV8) so Speed + Start + End can all be modulated
// at once. SPEED = through-zero varispeed (center unity, CCW through 0
// into reverse to -100%, CW +150%). START/END drive the CROP points: crop
// lives in the ENGINE (pure cursor math, CV-rate performable — no head
// rebuild, no SD, no menu repaint). No 1V/oct here by design: this machine
// is a clock-time loop recorder; instrument-style pitch playback (v/oct,
// ADSR, effects) is a separate future machine.

typedef struct {
    // -- assignment (reader-owned once load_req is raised) ------------------
    char name[S3_NAME_LEN];          // loaded sample id ("" = unloaded)
    volatile uint32_t file_frames;   // whole file length (frames)
    volatile bool  reverse;          // reader-side (chunks reversed on read)
    // reader streams the WHOLE file; crop is engine-side (below)
    volatile uint32_t play_start;    // 0 (kept for the reverse file mapping)
    volatile uint32_t play_len;      // == file_frames
    // -- CROP (engine-side, playback-order fractions of play_len; UI or the
    //    CV assignment writes these, the audio task only reads). Sampler2
    //    semantics: the params are START + LENGTH — start slides the WHOLE
    //    window (length preserved); when the end hits EOF the length gives
    //    way, and comes back as start retreats -------------------------------
    volatile float crop_start;       // 0..~0.98
    volatile float crop_len;         // window length, 0.02..1
    volatile float ui_cs, ui_ce;     // EFFECTIVE window this block (incl. CV),
                                     // published by the engine for the UI shade
    volatile float ui_cs_min, ui_cs_max;   // jitter meter: per-interval extremes
                                     // (menu reads + resets each slow tick)
    // -- buffers + cursors (deck protocol: reader owns wpos + seek-writes of
    //    rpos; audio task owns rpos during playback; volatile is enough for
    //    aligned 32-bit cursors on this core pair) --------------------------
    int16_t *head;                   // PSRAM, S3_HEAD_FRAMES * 2
    volatile uint32_t head_frames;   // valid playback-order frames in head
    volatile bool head_valid;        // false while (re)building the head
    int16_t *ring;                   // PSRAM, S3_RING_FRAMES * 2
    volatile uint32_t wpos;          // stream write head (playback-order, absolute)
    // play cursor: double so through-zero varispeed can run it backwards.
    // Audio task owns `pos` during playback; the reader writes it only while
    // the engine is parked (load, deck seek protocol). rpos_i is a per-block
    // integer mirror for the reader's throttle + the UI bar.
    double   pos;
    volatile uint32_t rpos_i;
    // -- request flags: UI/audio tasks ONLY set these; the reader clears and
    //    applies them (including any cursor rewrites) -----------------------
    volatile bool load_req;          // assign `pending` to this voice
    volatile bool autoplay;          // start playing once the load lands
                                     // (fresh takes loop immediately)
    volatile bool sync_start_req;    // start ON the next clock pulse, offset
                                     // by the elapsed save/load time so the
                                     // loop comes in IN PHASE with the clock
    char pending[S3_NAME_LEN];
    volatile bool window_req;        // reverse changed: rebuild head+stream
    volatile bool retrig_req;        // restart stream fill at seek_frame
    volatile uint32_t seek_frame;    // stream target (playback-order) — crop
                                     // starts beyond the head land here
    volatile uint32_t stream_cap;    // engine: don't stream past this frame
                                     // (0 = to EOF). Looping a crop window
                                     // must not race ahead and EVICT the
                                     // window from the ring — capped, the
                                     // window stays resident and wraps are
                                     // pure cursor math (seamless)
    // -- transport (audio-task-owned; loading is set by requesters and
    //    cleared by the reader, deck protocol) ------------------------------
    volatile bool playing;
    volatile bool loading;           // stream (re)filling; ring reads parked
    volatile int  playmode;          // S3_MODE_*
    // -- params (UI-owned, audio reads) --------------------------------------
    volatile int   src_speed;        // CV source per destination: -1 off,
    volatile int   src_start;        // 0..7 = CV1..CV8 (knobs 6/7 are the
    volatile int   src_len;          // fully-good knob+jack channels)
    volatile int   crop_mode;        // S3_CROP_*
    volatile float bpm;              // take tempo (sidecar stamp; 0 = unknown)
    volatile float level;            // 0..1
    volatile float pan;              // -1..1
    // -- engine-local ---------------------------------------------------------
    volatile float cur_rate;         // effective rate this block (UI badge)
    float out_gain;                  // declick ramp
    float last_l, last_r;            // decay-mute tail
    float cs_sm, ln_sm;              // slewed start/length: raw ADC noise made
                                     // the window jitter ~10s of ms on long
                                     // takes and re-snapped the playhead to
                                     // the start every few blocks
    int   q_cs, q_ln;                // QUANT: adopted start beat + length in
                                     // beats (hysteresis so noise can't
                                     // flutter between adjacent beats)
    uint32_t last_cs_f;              // window-motion detector: a fast start
    int   cs_moving;                 // sweep holds off unbuffered seeks (one
                                     // seek when it lands, not a storm)
    uint32_t starve_run;             // consecutive starved blocks — a run
                                     // past ~150ms force-seeks (wedge-proof)
    // -- waveform thumbnail (reader builds at load/window change; playback-
    //    order, so reverse mode shows it reversed for free) -------------------
    uint8_t wf[S3_WF_W];             // per-column peak, 0..255
    volatile bool wf_valid;
    // -- diagnostics ----------------------------------------------------------
    volatile uint32_t dbg_starve;    // blocks starved mid-play (reader behind)
    volatile uint32_t dbg_heal;      // self-heal seeks fired (audio task)
    volatile uint32_t dbg_retrig;    // retrig/seek requests consumed (reader)
} s3_voice_t;

typedef struct {
    s3_voice_t v[S3_NVOICES];
    volatile bool monitor;           // pass line-in through while armed+stopped
    volatile bool arm_mutes;         // arming a track mutes its playback
                                     // (sampler2 inheritance; toggleable)
    // recording UI state (mirrors the core recording service)
    volatile int  arm_target;        // -1 none, 0/1 = voice armed for recording
    volatile bool save_failed;       // last take failed to save (UI banner)
    char last_rec[S3_NAME_LEN];      // last auto-picked recording (UI)

    // CV clock: the SHARED conditioned front-end (clockin_t in
    // components/machine/clock.{h,c}) — Schmitt + floor + detector + ghost
    // gate + pulses-per-beat carried together. Drives the synced-record
    // workflow and the tempo stamp. PPQ is a Record-page setting (1/2/4/8).
    clockin_t ci;
    // ch1/2 idle ~21% up the scale by analog design (1V/oct jacks) — floor
    // trackers so matrix reads from them span the full range when patched
    int  cv12_floor[2];
    // per-channel median-of-5 conditioning: WiFi-burst ADC spikes (±80
    // counts on an idle knob) punch through slew and hysteresis — a median
    // eats impulses without lagging sustained moves
    uint16_t cv_hist[8][5];
    uint8_t  cv_hp;
    uint16_t cv_med[8];
    volatile int clk_src;            // CV channel index (default 7 = CV8)
    // internal clock: drives the synced-record workflow when no external
    // clock is locked (external always wins). 0 = off. 4 ppb, like ext.
    volatile float int_bpm;
    uint32_t int_since;              // frames since the last internal pulse

    // clock-synced capture (audio-task-owned; reader consumes stamp_req).
    // With a locked clock: capture STARTS on a pulse (downbeat = frame 0)
    // and STOPS on the next whole beat -> takes are loop-ready. The tempo
    // stamp lands in the take's JSN sidecar (deck convention bpm/grid/dver/
    // conf) so recordings arrive pre-analyzed for the deck (and give the
    // future crop mode its beat grid to snap crop points to).
    volatile int  rec_wait_vid;      // voice waiting for a pulse to start (-1 none)
    volatile bool rec_stop_wait;     // stop queued for the next whole beat
    volatile bool rec_synced;        // capture started on a pulse (grid = 0)
    volatile uint32_t rec_frames;    // frames captured so far
    volatile uint32_t rec_pulses;    // clock pulses since capture start
    volatile uint32_t rec_first_pulse; // frame of the first pulse (unsynced start)
    volatile float rec_bpm;          // beat bpm latched at finish (0 = no stamp)
    volatile bool rec_stamp_req;     // reader: amend the sidecar after pickup
    volatile uint32_t post_stop_frames; // frames since the synced stop edge —
                                     // the phase offset for sync_start_req
} s3_state_t;

extern s3_state_t s3;

// UI-side (SD-touching or flag-setting; call from UI/menu tasks only)
void s3_load_sample(int vid, const char *name);  // browse-assign (sets load_req)
void s3_set_reverse(int vid, bool reverse);      // reader rebuilds head+stream
void s3_toggle_arm(int vid);                     // explicit arm from the Record page
int  s3_list_samples(char (**names)[S3_NAME_LEN]); // usr/*.RAW browser list
