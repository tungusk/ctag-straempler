#pragma once
#include <stdint.h>
#include <stdbool.h>

// sample_ram — shared loading of SD library samples into a RAM buffer, so the
// slicer / granular (and future) machines don't each duplicate the file walk
// and the RAW format conversion. usr/*.RAW are int32-per-frame stereo-packed
// (L in low 16, R in high 16), 44.1 kHz.

// list usr/*.RAW sample ids (filename without extension) into out[][24].
// returns the count (<= max).
int sample_list(char out[][24], int max);

// shared browser list: usr/ can hold hundreds of samples, and each menu
// keeping its own static array both wastes DRAM and silently truncates (the
// old per-menu [32] caps hid fresh uploads, which land last in FAT order).
// Only one machine browses at a time, so every browser shares this buffer.
// Sorted case-insensitively; returns the count, points *out at the buffer.
#define SAMPLE_LIST_MAX 224
int sample_list_shared(char (**out)[24]);

// dated browser list: the same walk, but a 512-entry PSRAM buffer sorted
// NEWEST FIRST, so fresh takes/uploads land at the TOP of the browser and a
// grown library can't hide them behind the cap (when full, the oldest entry
// is evicted rather than the newest silently dropped). Timestamps come from
// f_readdir in the same pass — a per-file stat() would re-walk the directory
// once per entry. Shared buffer, like sample_list_shared: one browser at a
// time. Used by sampler3 + deck.
#define SAMPLE_LIST_RECENT_MAX 512
int sample_list_recent(char (**out)[24]);

// ---- folder dimension (the two-level browser) ---------------------------------
// The same three-dir walk, restricted to ONE folder — organization, not
// restriction: every file stays reachable, the folder screen just makes a
// grown library navigable and turns the list caps into per-folder budgets.
#define SAMPLE_DIR_ALL   (-1)
#define SAMPLE_DIR_POOL  0     // usr/ — imports, bounces, legacy
#define SAMPLE_DIR_REC   1     // usr/REC — takes
#define SAMPLE_DIR_LOOPS 2     // usr/LOOPS — looper saves
const char *sample_dir_name(int di);            // "all"/"pool"/"REC"/"LOOPS"
void sample_folder_counts(int out[3]);          // per-folder entry counts (display)
int sample_list_shared_dir(int di, char (**out)[24]);
int sample_list_recent_dir(int di, char (**out)[24]);

// load /sdcard/usr/<name>.RAW into dst. mono=false writes interleaved stereo
// (dst must hold max_frames*2 int16); mono=true averages L/R to one int16 per
// frame (dst holds max_frames). returns frames loaded, 0 on failure.
uint32_t sample_load(const char *name, int16_t *dst, uint32_t max_frames, bool mono);
