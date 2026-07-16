#ifndef PRESET_STORE_H
#define PRESET_STORE_H

// Named-preset store shared by machines (Synth #23, Keys, ...): one JSON file
// per preset in a per-machine directory, auto-numbered <PFX>NNN — 7-char ids,
// FatFS 8.3-safe (the card runs LFN-off), no on-device text entry (the
// firmware's REC_/BNC_ convention). Each machine feeds its own autosave
// (de)serializers through this; the store only owns mint/write/read/list.
// Every SD burst holds sd_lock (recursive, nested helper locks are fine).
// Call from UI/menu context only — never from a machine's process().

#include <stddef.h>
#include "cJSON.h"

typedef struct {
    const char *dir;   // VFS directory, e.g. "/sdcard/usr/synth" (mkdir'd on first save)
    const char *pfx;   // id prefix, e.g. "PAT_" -> ids PAT_000..PAT_999
} preset_store_t;

// Mint the next free id and write `state` to <dir>/<id>.jsn. TAKES OWNERSHIP
// of `state` (deleted before return; NULL is a failure). id_out (optional)
// receives the minted id. Returns 0 ok, <0 fail.
int preset_store_save(const preset_store_t *ps, cJSON *state, char *id_out, size_t n);

// Read a preset by id. Returns a cJSON the CALLER applies (its preset_load)
// and deletes, or NULL.
cJSON *preset_store_load(const preset_store_t *ps, const char *id);

// Fill ids[] NEWEST FIRST (ids are zero-padded, so string order == numeric
// order). Returns the count (<= max).
int preset_store_list(const preset_store_t *ps, char ids[][12], int max);

#endif
