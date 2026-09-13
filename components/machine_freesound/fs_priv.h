#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Freesound machine. The download → decode → install pipeline lands a preview
// MP3 in the usr/ sample library (mono previews expanded to stereo); one
// pipeline at a time, and the pipeline task only touches this static state, so
// it survives a machine switch mid-run.
//
// 2026-09-12: the machine is no longer web-only. fs_search_start() runs the
// same text search the web proxy runs, but PARSES it on the device into
// results[] so the panel can browse it, and the query store below keeps recent
// and saved queries so typing on the encoder is the exception, not the rule.
// GET /fs/search still proxies raw JSON — the browser wants to parse it itself.

enum { FS_IDLE = 0, FS_DOWNLOAD, FS_DECODE, FS_INSTALL, FS_DONE, FS_ERROR };
enum { FS_SEARCH_IDLE = 0, FS_SEARCH_RUNNING, FS_SEARCH_OK, FS_SEARCH_ERR };

#define FS_RESULTS_MAX  16      // == the page_size asked of the API
#define FS_QUERY_LEN    48      // == TEXT_ENTRY_MAX + 1
#define FS_RECENTS      6
#define FS_SAVED        8

typedef struct {
    char     id[12];
    char     name[40];
    char     user[20];
    uint16_t dur_ds;            // duration in TENTHS of a second (0.1 s is plenty)
} fs_result_t;

#define FS_MAX_SECONDS   90                 // preview length cap: keeps RAW conversions SD-sane
#define FS_MAX_MP3_BYTES (6 * 1024 * 1024)  // direct-URL guard (no duration known up front)

typedef struct {
    volatile int  phase;         // FS_*
    volatile int  progress;      // 0..100 within the current phase
    volatile bool busy;          // pipeline task alive
    char cur_id[16];
    char cur_name[24];
    char err[64];
    char last_query[FS_QUERY_LEN];   // persisted in the preset

    // ---- panel search results (owned by the search task, read by the UI) ----
    volatile int search_state;       // FS_SEARCH_*
    volatile int n_results;          // valid entries in results[]
    volatile int page;               // 1-based page currently held
    volatile int total;              // total hits freesound reports
    fs_result_t  results[FS_RESULTS_MAX];
    char         serr[48];
    volatile unsigned search_stack_min;   // same instrument as stack_min, own task

    // ---- query store (persisted) -------------------------------------------
    char recents[FS_RECENTS][FS_QUERY_LEN];
    int  n_recents;
    char saved[FS_SAVED][FS_QUERY_LEN];
    int  n_saved;
    bool autoplay;                   // hear a fetch the moment it lands
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
void fs_safe_name(const char *raw, const char *id, char *out, size_t n);  // pool-safe take id

// ---- panel search ----------------------------------------------------------
// Runs in its own task: a TLS session is ~20 KB of stack and must not go on the
// UI task, and hanging it off the pipeline task would put cJSON on top of the
// 28 KB peak that already panicked this module once (see fs_machine.c).
// -1 = a search or a download is already running.
int  fs_search_start(const char *q, int page);
const char *fs_search_err(void);

// ---- query store -----------------------------------------------------------
// Recents are pushed automatically (most recent first, de-duplicated); saved
// queries are pinned by hand from Setup. Both persist in the machine preset.
void fs_query_remember(const char *q);
int  fs_query_save(const char *q);      // 0 ok, -1 full, -2 already saved
void fs_query_unsave(const char *q);
bool fs_query_is_saved(const char *q);

// ---- audition --------------------------------------------------------------
// A fetched preview is already in the pool, so "hear it" is streaming the take
// we just wrote (util/sampplay) rather than a second network path. Keep/drop is
// what makes that honest: drop removes the take and its sidecar.
int  fs_audition(const char *name);     // stream usr/<name>, looped; 0 ok
void fs_audition_stop(void);
bool fs_auditioning(void);
const char *fs_audition_name(void);
int  fs_audition_drop(void);            // stop + delete the take; 0 ok
