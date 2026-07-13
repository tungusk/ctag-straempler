#pragma once
#include <stdint.h>
#include <stdbool.h>

// sampimport — convert-on-import (2026-07-13): turn any audio file the pool
// can't natively play into a native 16-bit/44.1 kHz stereo .WAV, ONCE.
//
//   convertible: MP3 (helix decoder), WAV PCM 8/16/24-bit + 32-bit float at
//                any sample rate 8..96 kHz, AIFF PCM 16/24-bit any rate,
//                mono or stereo
//   pipeline:    decode -> int16 stereo -> streaming cubic resampler ->
//                sampwav writer, chunked with sd_lock per burst + yields
//   rule:        the source is REPLACED by its native twin (CHAIN.MP3 becomes
//                CHAIN.WAV; a 48k CHAIN.WAV is rewritten in place) — no name
//                collisions, no double listings, card space stays sane
//
// samp_import_file runs in the CALLER's task (long: seconds per minute of
// audio). samp_import_start spawns the pool-wide scan in its own task and
// returns immediately; poll the progress globals (REST /import does).

// convert one file (VFS path). Returns 1 = already native (untouched),
// 0 = converted (source replaced), -1 = failed / not convertible.
int samp_import_file(const char *vfs_path);

// spawn a scan-and-convert pass over every pool folder. Returns 0, or -1 if
// one is already running.
int samp_import_start(void);

// progress (scan task publishes; UI/REST read)
extern volatile bool samp_import_busy;
extern volatile int  samp_import_done;    // files converted this pass
extern volatile int  samp_import_fail;    // files that failed conversion
extern volatile int  samp_import_seen;    // candidates encountered (diagnostic)
extern volatile int  samp_import_pct;     // 0..100 within the current file
extern char samp_import_cur[32];          // file currently converting
