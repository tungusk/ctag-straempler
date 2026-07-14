#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "clock.h"

// Tracker — a multi-format module player (MOD/XM/IT/S3M/669/… via libxmp).
// Architecture mirrors the deck: an unpinned RENDER task owns the libxmp
// context end-to-end (create/load/render/free) and fills a PSRAM ring;
// process() only consumes the ring, reads gates, and runs the CV clock
// detector. No libxmp or SD call ever happens in the audio task.

#define TRK_RATE         44100
#define TRK_RING_FRAMES  (TRK_RATE * 2)       // 2 s stereo int16 ring (~352 KB PSRAM)
#define TRK_LOW_WATER    (TRK_RATE / 4)       // unmute once ~0.25 s is buffered
#define TRK_CHUNK        1024                 // frames per xmp_play_buffer call
#define TRK_MAX_FILE     (2 * 1024 * 1024)    // reject modules bigger than this
// modules live in their own folder so they don't mingle with the RAW sample
// library. TRK_DIR_VFS is for opendir/fopen; TRK_DIR_FAT is the bare FatFS
// path for f_open/f_mkdir (no /sdcard prefix — see CLAUDE.md).
#define TRK_DIR_VFS      "/sdcard/usr/MODS"
#define TRK_DIR_FAT      "/usr/MODS"
#define TRK_NAME_LEN     24                   // 8.3 filename incl. extension
#define TRK_TITLE_LEN    40
#define TRK_MAX_NAMES    48                   // captured sample/instrument names
#define TRK_NM_LEN       24                   // per name (libxmp gives up to 31)
#define TRK_MAX_ORDERS   256                  // order-list map for cross-pattern loop

enum { TRK_EMPTY = 0, TRK_LOADING, TRK_READY, TRK_FAIL };

typedef struct {
    // streaming ring (render task writes, process reads)
    int16_t *ring;                 // PSRAM, TRK_RING_FRAMES * 2
    volatile uint32_t wpos;        // render write head (frames, absolute)
    volatile uint32_t rpos;        // play cursor (frames, absolute)
    volatile bool playing;
    volatile bool loading;         // render (re)filling after load/seek — process mutes

    // request protocol: UI/audio ONLY set these flags; the render task is the
    // sole caller of libxmp and the sole writer of wpos/rpos (mirrors the
    // deck's hard-won seek protocol — concurrent context access = crash).
    volatile bool load_req;
    char pending[TRK_NAME_LEN];    // filename (incl. ext) to load
    volatile bool seek_req;
    volatile int  seek_pos;        // target pattern-order position
    volatile bool restart_req;
    volatile int  nudge_req;       // STEP-NUDGE (sync mode): pending encoder
                                   // detents; render applies whole-pulse row
                                   // jumps on a row boundary. UI adds, render
                                   // consumes; cleared on load/restart/scrub/
                                   // sync-drop so stale detents never fire.

    // published module snapshot (render task writes after each render)
    volatile int  state;           // TRK_*
    char  title[TRK_TITLE_LEN];    // internal module title (fallback: filename)
    char  fmt[20];                 // e.g. "Protracker", "Fast Tracker II"
    char  fail_why[24];
    // sample/instrument names — composers often hide the song's message/credits
    // here. Captured once at load; read by the UI (scrolled on knob7/CV7).
    char  names[TRK_MAX_NAMES][TRK_NM_LEN];
    int   n_names;
    volatile int  channels;
    volatile int  cur_pos, cur_pat, cur_row, num_pat;
    volatile int  time_ms, total_ms;
    volatile int  mod_bpm;         // module nominal BPM (captured at tf=1.0)
    volatile int  cur_bpm;         // current effective BPM (frame_info)

    // settings (persisted)
    char  file[TRK_NAME_LEN];
    volatile bool loop;
    volatile bool sync;            // follow the external CV clock
    volatile bool amiga;           // Amiga (nearest+wide) vs Clean (spline+narrow)
    volatile bool show_text;       // Live page shows the sample-name message panel
    volatile int  clk_src;         // CV channel of the clock
    volatile int  ppb_idx;         // pulses-per-beat index into trk_ppb[]
    volatile bool sound_dirty;     // menu flipped amiga → render re-applies

    // --- sequence loop ("loop mode": a live step-region loop, PO/KO-II style) ---
    // process() sets loop_len/loop_pos_cv from CV7/CV6 and raises loop_toggle_req
    // on a TR2 edge; the render task owns loop_engage + the transition (snapshot
    // on engage, xmp_seek_time to the phantom position on release).
    volatile bool loop_engage;     // loop mode active
    volatile bool loop_toggle_req; // TR2 edge → render flips loop_engage
    volatile bool loop_freeze;     // release: freeze (resume at loop) vs keep-running (setting)
    volatile int  loop_len;        // window length in steps/rows (CV7 selector)
    // RETRIG — the sub-step rungs of the CV7 ladder (Arlo: "is it possible to go
    // sub 1 step on the looper and make it into a retrig under there? ... kind of
    // like the old old sampler used to"). Below one step the SCORE has no finer
    // address (libxmp seeks to a row at best), so the retrig happens in the AUDIO
    // domain instead: the play cursor stops advancing and wraps a short window of
    // the rendered ring. Freezing the cursor also stalls the renderer, so libxmp's
    // clock stops with it — the window cannot be overwritten and the song cannot
    // drift underneath the stutter.
    volatile int  retrig_div;      // 0 = off, else 2/4/8/16 = 1/2, 1/4, 1/8, 1/16 step
    volatile uint32_t retrig_len;  // the live window, in frames (UI)
    volatile int  loop_pos_cv;     // CV6 raw 0..4095 → position block across the song
    volatile int  loop_start_ord;  // render-published window origin (UI): order...
    volatile int  loop_start_row;  //  ...and row within it
    volatile int  loop_a_pm;       // loop window start as per-mille of the song (bar)
    volatile int  loop_b_pm;       // loop window end as per-mille of the song (bar)
    // order-list map (built at load) so the loop window can span patterns
    uint16_t order_rows[TRK_MAX_ORDERS];   // rows per order position
    uint32_t order_step0[TRK_MAX_ORDERS];  // cumulative absolute step at each order
    int      n_orders;
    uint32_t total_steps;          // sum of all order rows = song length in steps

    // CV clock (process fills, render reads for sync) — the shared
    // conditioned front-end (clock.h): floor-tracked Schmitt + ppb-scaled
    // gates replace the private near-copy of the deck pattern
    clockin_t ci;
    float tf_cur;                  // current tempo factor (render-owned)
    int   ph_row, ph_frame, ph_speed;  // last rendered row/tick position
                                       // (render-owned; feeds the sync phase pull)

    float out_gain;                // declick ramp (0..1)
    volatile uint32_t dbg_starve;  // blocks muted mid-play: render fell behind
} trk_state_t;

extern trk_state_t trk;
extern const float trk_ppb[5];
// RAW ppb feeds the detector's sanity gates; EFFECTIVE (x the octave fold)
// feeds tempo math — mixing them makes the fold move the gates and the lock
// never settles (bench-caught on the deck).
#define TRK_PPB_RAW() (trk_ppb[trk.ppb_idx])
#define TRK_PPB_EFF() (trk_ppb[trk.ppb_idx] * (trk.ci.oct > 0 ? trk.ci.oct : 1.0f))
extern const char *const trk_ppb_names[5];

// render-task-only libxmp entry (implemented in tracker.c, called by menu)
int  tracker_list_modules(char (**out)[TRK_NAME_LEN]);   // browser list
void tracker_request_load(const char *name);
void tracker_toggle_play(void);
