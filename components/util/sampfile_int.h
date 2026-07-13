#pragma once
// sampfile internals — the container parsers, written against a read-at
// callback so the VFS TU (sampfile.c) and the raw-FatFS TU (sampfile_f.c)
// share ONE parser without sharing headers (ff.h and dirent.h both typedef
// DIR — the house rule that keeps those worlds in separate files).
#include <string.h>
#include "sampfile.h"

#define SF_RATE 44100

// read n bytes at absolute offset off; returns bytes read
typedef size_t (*sf_read_at_fn)(void *ctx, long off, void *buf, size_t n);

static inline uint32_t sf_le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | ((uint32_t)p[3] << 24); }
static inline uint32_t sf_le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static inline uint32_t sf_rb32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | p[1] << 16 | p[2] << 8 | p[3]; }
static inline uint32_t sf_rb16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

// AIFF stores the sample rate as an 80-bit extended float
static inline uint32_t sf_ext80(const uint8_t *p)
{
    int exp = (int)(((p[0] & 0x7F) << 8) | p[1]) - 16383;
    uint32_t hi = sf_rb32(p + 2);           // top mantissa bits (bit 31 = integer bit)
    if (exp < 0 || exp > 31) return 0;
    return hi >> (31 - exp);
}

static inline int sf_wav_parse(sampfile_t *sf, long fsize, sf_read_at_fn rd, void *ctx)
{
    uint8_t h[24];
    bool have_fmt = false;
    uint32_t rate = 0, bits = 0, ch = 0, acode = 0;
    long pos = 12;                           // past RIFF....WAVE
    while (pos + 8 <= fsize) {
        if (rd(ctx, pos, h, 8) != 8) break;
        uint32_t csz = sf_le32(h + 4);
        if (memcmp(h, "fmt ", 4) == 0 && csz >= 16) {
            if (rd(ctx, pos + 8, h, 16) != 16) break;
            acode = sf_le16(h);
            ch    = sf_le16(h + 2);
            rate  = sf_le32(h + 4);
            bits  = sf_le16(h + 14);
            have_fmt = true;
        } else if (memcmp(h, "data", 4) == 0) {
            if (!have_fmt)          { sf->why = "WAV: data before fmt"; return -1; }
            sf->src_rate = rate; sf->src_bits = (uint16_t)bits;
            sf->src_code = (uint16_t)acode; sf->src_ch = (uint16_t)ch;
            sf->src_data_off = (uint32_t)(pos + 8);
            sf->src_data_len = ((long)pos + 8 + (long)csz > fsize || csz == 0 ||
                                csz == 0xFFFFFFFFu)
                                   ? (uint32_t)(fsize - pos - 8) : csz;
            if (acode != 1)         { sf->why = "WAV: not plain PCM"; return -1; }
            if (bits != 16)         { sf->why = "WAV: not 16-bit"; return -1; }
            if (rate != SF_RATE)    { sf->why = "WAV: not 44.1kHz"; return -1; }
            if (ch != 1 && ch != 2) { sf->why = "WAV: channels"; return -1; }
            uint32_t dsz = csz;
            // self-heal placeholder/truncated sizes (power-cut recordings):
            // trust the file itself
            if (dsz == 0 || dsz == 0xFFFFFFFFu ||
                (long)pos + 8 + (long)dsz > fsize)
                dsz = (uint32_t)(fsize - pos - 8);
            sf->fmt = SF_WAV;
            sf->channels = (uint8_t)ch;
            sf->be = false;
            sf->data_off = (uint32_t)(pos + 8);
            sf->frames = dsz / (ch * 2);
            return 0;
        }
        pos += 8 + csz + (csz & 1);          // chunks pad to even
    }
    sf->why = "WAV: no data chunk";
    return -1;
}

static inline int sf_aiff_parse(sampfile_t *sf, long fsize, sf_read_at_fn rd, void *ctx)
{
    uint8_t h[26];
    bool have_comm = false;
    uint32_t ch = 0, bits = 0, rate = 0, nframes = 0;
    long pos = 12;                           // past FORM....AIFF
    while (pos + 8 <= fsize) {
        if (rd(ctx, pos, h, 8) != 8) break;
        uint32_t csz = sf_rb32(h + 4);
        if (memcmp(h, "COMM", 4) == 0 && csz >= 18) {
            if (rd(ctx, pos + 8, h, 18) != 18) break;
            ch      = sf_rb16(h);
            nframes = sf_rb32(h + 2);
            bits    = sf_rb16(h + 6);
            rate    = sf_ext80(h + 8);
            have_comm = true;
        } else if (memcmp(h, "SSND", 4) == 0) {
            if (!have_comm)         { sf->why = "AIFF: SSND before COMM"; return -1; }
            sf->src_rate = rate; sf->src_bits = (uint16_t)bits;
            sf->src_code = 1; sf->src_ch = (uint16_t)ch;
            sf->be = true;              // AIFF is big-endian even when rejected
            {
                uint8_t sh[8];
                if (rd(ctx, pos + 8, sh, 8) != 8) break;  // offset + blocksize
                uint32_t off = sf_rb32(sh);
                sf->src_data_off = (uint32_t)(pos + 8 + 8 + off);
                sf->src_data_len = (uint32_t)(fsize - sf->src_data_off);
            }
            if (bits != 16)         { sf->why = "AIFF: not 16-bit"; return -1; }
            if (rate != SF_RATE)    { sf->why = "AIFF: not 44.1kHz"; return -1; }
            if (ch != 1 && ch != 2) { sf->why = "AIFF: channels"; return -1; }
            sf->fmt = SF_AIFF;
            sf->channels = (uint8_t)ch;
            sf->be = true;
            sf->data_off = sf->src_data_off;
            uint32_t avail = (uint32_t)(fsize - sf->data_off) / (ch * 2);
            sf->frames = (nframes && nframes <= avail) ? nframes : avail;
            return 0;
        }
        pos += 8 + csz + (csz & 1);
    }
    sf->why = "AIFF: no SSND chunk";
    return -1;
}

// dispatch on the magic bytes; RAW is the headerless fallback
static inline int sf_parse(sampfile_t *sf, long fsize, sf_read_at_fn rd, void *ctx)
{
    memset(sf, 0, sizeof(*sf));
    sf->why = "";
    uint8_t magic[12];
    if (fsize >= 44 && rd(ctx, 0, magic, 12) == 12) {
        if (memcmp(magic, "RIFF", 4) == 0 && memcmp(magic + 8, "WAVE", 4) == 0)
            return sf_wav_parse(sf, fsize, rd, ctx);
        if (memcmp(magic, "FORM", 4) == 0 && memcmp(magic + 8, "AIFF", 4) == 0)
            return sf_aiff_parse(sf, fsize, rd, ctx);
        if (memcmp(magic, "FORM", 4) == 0 && memcmp(magic + 8, "AIFC", 4) == 0) {
            sf->why = "AIFC (compressed) unsupported";
            return -1;
        }
    }
    sf->fmt = SF_RAW;                        // headerless native
    sf->channels = 2;
    sf->be = false;
    sf->data_off = 0;
    sf->frames = (uint32_t)(fsize / 4);
    return 0;
}

// shared post-read conversion: swap AIFF bytes, expand mono in place
// (backward walk: source index <= destination index, so it's safe).
// dst holds `got` samples-or-frames as read; afterwards it holds `got`
// native stereo frames.
static inline void sf_convert(const sampfile_t *sf, int16_t *dst, size_t got)
{
    if (sf->be) {
        size_t nsamp = (sf->channels == 2) ? got * 2 : got;
        uint8_t *b = (uint8_t *)dst;
        for (size_t k = 0; k < nsamp; k++) {
            uint8_t t = b[k * 2];
            b[k * 2] = b[k * 2 + 1];
            b[k * 2 + 1] = t;
        }
    }
    if (sf->channels == 1) {
        for (size_t k = got; k-- > 0; ) {
            int16_t v = dst[k];
            dst[k * 2] = v;
            dst[k * 2 + 1] = v;
        }
    }
}
