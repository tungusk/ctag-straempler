#pragma once
#include <stdbool.h>
#include <stddef.h>

// Auth seam: everything that needs freesound credentials goes through here,
// so the planned OAuth2 upgrade (bearer header + refresh flow, needed for
// originals/upload) replaces this file's internals without touching the
// search/download code. Today it wraps the CONFIG.JSN API key.

bool fs_auth_ok(void);                             // credentials present?
int  fs_auth_query_suffix(char *buf, size_t len);  // "&token=..." today; -1 if none
