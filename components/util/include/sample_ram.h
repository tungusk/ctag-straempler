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

// load /sdcard/usr/<name>.RAW into dst. mono=false writes interleaved stereo
// (dst must hold max_frames*2 int16); mono=true averages L/R to one int16 per
// frame (dst holds max_frames). returns frames loaded, 0 on failure.
uint32_t sample_load(const char *name, int16_t *dst, uint32_t max_frames, bool mono);
