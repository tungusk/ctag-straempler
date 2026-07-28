#pragma once
#include <stdint.h>
#include <stdbool.h>

// Freesound machine — a silent utility machine whose value is its web surface:
// GET /fs/search proxies the freesound text search, POST /fs/get runs a
// download → decode → install pipeline that lands the preview MP3 in the usr/
// sample library (mono previews expanded to stereo), GET /fs/state reports
// pipeline progress for polling. One pipeline at a time; the pipeline task
// only touches this static state, so it survives a machine switch mid-run.

enum { FS_IDLE = 0, FS_DOWNLOAD, FS_DECODE, FS_INSTALL, FS_DONE, FS_ERROR };

#define FS_MAX_SECONDS   90                 // preview length cap: keeps RAW conversions SD-sane
#define FS_MAX_MP3_BYTES (6 * 1024 * 1024)  // direct-URL guard (no duration known up front)

typedef struct {
    volatile int  phase;         // FS_*
    volatile int  progress;      // 0..100 within the current phase
    volatile bool busy;          // pipeline task alive
    char cur_id[16];
    char cur_name[24];
    char err[64];
    char last_query[64];         // persisted in the preset
    volatile unsigned stack_min; // smallest free stack seen in the pipeline task,
                                 // in BYTES. The pipeline runs two TLS sessions,
                                 // a cJSON parse and SD writes; when it panicked
                                 // there was no way to tell a stack overflow from
                                 // a bad pointer without a serial cable.
} fs_state_t;

extern fs_state_t fsm;

const char *fs_phase_name(int phase);
int  fs_http_get(const char *url, char **out, int max_len);  // PSRAM buffer, caller frees
int  fs_get_start(const char *id, const char *name);         // spawn pipeline; -1 if busy/failed
int  fs_fetch_start(const char *url, const char *name);      // same, from a direct http(s) MP3 URL
