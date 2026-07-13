#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

// sampfile — the ONE format seam between the sample pool and every reader
// (2026-07-13 WAV/AIFF feasibility build). The pool's native currency stays
// 44.1 kHz interleaved int16 stereo; this layer lets that currency live in
// three containers:
//
//   .RAW          headerless native frames (the freesound-era format)
//   .WAV          RIFF PCM 16-bit 44.1 kHz, mono or stereo (stereo data is
//                 byte-identical to RAW — readers keep their fast path)
//   .AIF/.AIFF    FORM/COMM/SSND PCM 16-bit 44.1 kHz, mono or stereo
//                 (big-endian: swapped at read)
//
// Anything else (non-PCM, non-16-bit, non-44.1k) is REJECTED at probe with a
// reason string — silence with no explanation reads as a hardware fault.
// All conversion (offset, mono expand, byteswap) happens in reader tasks /
// UI loaders; audio tasks keep consuming native frames from rings.
//
// Ids stay extension-less everywhere (browsers, sidecars, presets):
// sample_resolve() maps an id to whichever container exists, .RAW winning
// ties so no existing pair ever changes behaviour.

typedef enum { SF_RAW = 0, SF_WAV, SF_AIFF } sf_fmt_t;

typedef struct {
    sf_fmt_t fmt;
    uint32_t data_off;      // byte offset of frame 0
    uint32_t frames;        // total frames (from the chunks, not size/4)
    uint8_t  channels;      // 1 or 2
    bool     be;            // big-endian samples (AIFF)
    const char *why;        // rejection reason when probe fails (static str)
} sampfile_t;

// bytes per frame in the FILE (2 for mono, 4 for stereo)
static inline uint32_t sf_stride(const sampfile_t *sf)
{
    return sf->channels == 1 ? 2u : 4u;
}
// byte position of a frame in the file
static inline long sf_seek_pos(const sampfile_t *sf, uint32_t frame)
{
    return (long)sf->data_off + (long)frame * (long)sf_stride(sf);
}
// true when file bytes ARE native frames (RAW / stereo-16 WAV): callers may
// fread straight into their staging buffer and skip sampfile_read entirely
static inline bool sf_native(const sampfile_t *sf)
{
    return !sf->be && sf->channels == 2;
}

// parse the container (extension-agnostic: sniffs magic). Returns 0 and
// fills sf, or -1 with sf->why set. Leaves the file positioned at frame 0.
int sampfile_probe(FILE *f, sampfile_t *sf);

// read up to n frames starting at the CURRENT file position into native
// interleaved int16 stereo (dst holds 2*n int16). Returns frames delivered.
// Handles mono expansion and AIFF byteswap; for sf_native() files it is a
// plain fread. The caller owns seeking (sf_seek_pos) and any sd_lock.
size_t sampfile_read(FILE *f, const sampfile_t *sf, int16_t *dst, size_t n);

// id -> actual pool path. Tries usr/<id>.RAW, .WAV, .AIF, .AIFF (that
// order — RAW keeps priority). Returns 0 and writes the VFS path, or -1.
int sample_resolve(const char *id, char *path, size_t path_len);

// list-side helper: does this directory entry name look like a pool sample?
// (any supported extension). Writes the extension-less id into id_out.
bool sample_name_id(const char *fname, char *id_out, size_t id_len);
