// Run a function on the UI task (the one that owns machine state, menus and
// the machine switch) and wait for its result — for web handlers that must
// read machine state consistently (GET /remote/params ran preset_save on the
// httpd task while the UI task loaded or switched under it).
#pragma once
#include <stdbool.h>

// fn runs on the UI task; its result comes back here. On timeout (UI task busy
// in a long load, or the queue full) returns NULL with *ran = false and the
// caller should fall back; a result produced after the timeout is handed to
// late_free (may be NULL) instead.
void *ui_call_sync(void *(*fn)(void), void (*late_free)(void *), int timeout_ms, bool *ran);
