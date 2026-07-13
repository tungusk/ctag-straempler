// Elektron Octatrack .ot slice-sidecar I/O.
//
// The .ot is an 832-byte (0x340) big-endian binary next to the sample:
//   header[16]  @0x000  "FORM\0\0\0\0DPS1SMPA"
//   unknown[7]  @0x010  {0,0,0,0,0,2,0}
//   tempo   u32 @0x017  bpm * 24
//   trimLen u32 @0x01B  bars * 100
//   loopLen u32 @0x01F  bars * 100
//   stretch u32 @0x023  0=off
//   loop    u32 @0x027  0=off
//   gain    u16 @0x02B  0x30 = 0 dB
//   quant   u8  @0x02D  0xFF = direct
//   trimStart u32 @0x02E, trimEnd u32 @0x032, loopPoint u32 @0x036
//   slices[64] {start,end,loop} u32 @0x03A   (loop 0xFFFFFFFF = none)
//   sliceCount u32 @0x33A
//   checksum   u16 @0x33E  = 16-bit sum of bytes 0x10..0x33D
//
// Export constants marked (v) are best-known from community docs — verify
// against a file exported by a real Octatrack before trusting them blindly;
// import only depends on the header/offsets, which are solid.
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "sd_lock.h"
#include "slicer_priv.h"

#define OT_SIZE      832
#define OT_SLICES_AT 0x3A
#define OT_COUNT_AT  0x33A
#define OT_CSUM_AT   0x33E

static const uint8_t OT_HEADER[16] =
    { 'F','O','R','M', 0,0,0,0, 'D','P','S','1','S','M','P','A' };

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }

static uint16_t ot_checksum(const uint8_t *b) {
    uint16_t s = 0;
    for (int i = 0x10; i < OT_CSUM_AT; i++) s += b[i];
    return s;
}

int slicer_parse_ot(const char *name, uint32_t sample_len, uint32_t *out_pt, int max_pts)
{
    if (!name || !name[0] || sample_len == 0 || max_pts < 2) return -1;

    char path[64];
    snprintf(path, sizeof(path), "/sdcard/usr/%s.OT", name);
    uint8_t b[OT_SIZE];
    sd_lock_take();
    FILE *f = fopen(path, "rb");
    size_t got = 0;
    if (f) { got = fread(b, 1, OT_SIZE, f); fclose(f); }
    sd_lock_give();
    if (!f || got != OT_SIZE) return -1;
    if (memcmp(b, OT_HEADER, sizeof(OT_HEADER)) != 0) {
        ESP_LOGW("SL-OT", "%s: bad header", path);
        return -1;
    }
    uint16_t want = ((uint16_t)b[OT_CSUM_AT] << 8) | b[OT_CSUM_AT + 1];
    if (want != ot_checksum(b))
        ESP_LOGW("SL-OT", "%s: checksum mismatch (using anyway)", path);

    uint32_t count = be32(b + OT_COUNT_AT);
    if (count == 0 || count > SL_OT_SLICES) return 0;

    // scale by the .ot's own reference length so a resampled RAW still lines
    // up: prefer trimEnd, fall back to the last slice end, then 1:1
    uint32_t ref = be32(b + 0x32);
    if (ref == 0) ref = be32(b + OT_SLICES_AT + (count - 1) * 12 + 4);
    if (ref == 0) ref = sample_len;
    // TRUNCATION clamp (long-chain phase 1): a chain longer than the RAM cap
    // loads cut short, and its .ot ref then dwarfs sample_len — the old scale
    // COMPRESSED the whole slice map into the loaded stub. A mismatch this
    // big means truncation, not resampling: keep 1:1 and DROP slices past
    // the cut instead.
    bool truncated = (uint64_t)ref > (uint64_t)sample_len + sample_len / 5;

    int n = 0;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < count && n < max_pts - 1; i++) {
        uint32_t start = be32(b + OT_SLICES_AT + i * 12);
        if (truncated && start >= sample_len) continue;   // beyond the cut: drop
        uint32_t pt = truncated ? start
                                : (uint32_t)(((uint64_t)start * sample_len) / ref);
        if (pt >= sample_len) pt = sample_len - 1;
        if (n > 0 && pt <= prev) continue;     // force monotonic
        out_pt[n++] = pt;
        prev = pt;
    }
    if (n == 0) return 0;
    if (out_pt[0] != 0) {                       // slices must start the sample
        if (n >= max_pts - 1) n = max_pts - 2;
        memmove(out_pt + 1, out_pt, (size_t)n * sizeof(uint32_t));
        out_pt[0] = 0;
        n++;
    }
    out_pt[n] = sample_len;                     // final boundary
    ESP_LOGI("SL-OT", "%s: %d slices (ref %lu -> len %lu)",
             path, n, (unsigned long)ref, (unsigned long)sample_len);
    return n;
}

int slicer_build_ot(uint8_t out[OT_SIZE], float bpm)
{
    if (sl.len == 0 || sl.n_slices < 1) return -1;
    if (sl.n_slices > SL_OT_SLICES) return -1;   // Octatrack tops out at 64
    if (bpm <= 0) bpm = 120.0f;

    memset(out, 0, OT_SIZE);
    memcpy(out, OT_HEADER, sizeof(OT_HEADER));
    static const uint8_t unk[7] = { 0, 0, 0, 0, 0, 2, 0 };   // (v)
    memcpy(out + 0x10, unk, sizeof(unk));

    float beats = (float)sl.len * bpm / (44100.0f * 60.0f);
    uint32_t bars100 = (uint32_t)(beats / 4.0f * 100.0f + 0.5f);
    if (bars100 == 0) bars100 = 25;              // min 1/4 bar
    put_be32(out + 0x17, (uint32_t)(bpm * 24.0f + 0.5f));   // tempo = bpm*24 (v)
    put_be32(out + 0x1B, bars100);               // trimLen, bars*100 (v)
    put_be32(out + 0x1F, bars100);               // loopLen
    put_be32(out + 0x23, 0);                     // stretch off
    put_be32(out + 0x27, 0);                     // loop off
    put_be16(out + 0x2B, 0x30);                  // gain 0 dB (v)
    out[0x2D] = 0xFF;                            // quantize: direct (v)
    put_be32(out + 0x2E, 0);                     // trimStart
    put_be32(out + 0x32, sl.len);                // trimEnd
    put_be32(out + 0x36, 0);                     // loopPoint

    for (int i = 0; i < sl.n_slices; i++) {
        uint8_t *s = out + OT_SLICES_AT + i * 12;
        put_be32(s,     sl.slice_pt[i]);         // start
        put_be32(s + 4, sl.slice_pt[i + 1]);     // end = next start (contiguous)
        put_be32(s + 8, 0xFFFFFFFF);             // no slice loop
    }
    put_be32(out + OT_COUNT_AT, (uint32_t)sl.n_slices);
    put_be16(out + OT_CSUM_AT, ot_checksum(out));
    return 0;
}
