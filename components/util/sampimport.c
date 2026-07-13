// sampimport — convert-on-import. See sampimport.h for the contract.
// VFS TU (fopen/dirent); MP3 decode goes through helix (FatFS paths — the
// decoder is handed BARE paths derived from ours).
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sd_lock.h"
#include "sampfile.h"
#include "mp3.h"
#include "sampimport.h"

static const char *TAG = "IMPORT";

volatile bool samp_import_busy = false;
volatile int  samp_import_done = 0;
volatile int  samp_import_fail = 0;
volatile int  samp_import_seen = 0;
volatile int  samp_import_pct = 0;
char samp_import_cur[32] = "";

#define IMP_CHUNK 2048      // source frames per burst

// ---- source frame reader: any supported PCM layout -> int16 stereo ----------
typedef struct {
    FILE *f;
    uint32_t rate, frames, read;
    uint16_t ch, bits, code;   // code 3 = float32
    bool be;
    long data_off;
} imp_src_t;

// read up to n frames into dst (int16 interleaved stereo). Caller holds no lock.
static size_t src_read(imp_src_t *s, int16_t *dst, size_t n, uint8_t *raw)
{
    if (s->read >= s->frames) return 0;
    if (n > s->frames - s->read) n = s->frames - s->read;
    uint32_t bpf = (s->bits / 8) * s->ch;             // bytes per source frame
    sd_lock_take();
    size_t got = fread(raw, bpf, n, s->f);
    sd_lock_give();
    for (size_t k = 0; k < got; k++) {
        int32_t v[2] = {0, 0};
        for (int c = 0; c < s->ch; c++) {
            const uint8_t *p = raw + k * bpf + c * (s->bits / 8);
            if (s->code == 3) {                        // float32 LE
                float fv;
                memcpy(&fv, p, 4);
                if (fv > 1.0f) fv = 1.0f;
                if (fv < -1.0f) fv = -1.0f;
                v[c] = (int32_t)(fv * 32767.0f);
            } else if (s->bits == 24) {
                v[c] = s->be ? (int32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8)) >> 8
                             : (int32_t)((p[2] << 24) | (p[1] << 16) | (p[0] << 8)) >> 8;
                v[c] >>= 8;                            // 24 -> 16
            } else if (s->bits == 8) {                 // WAV 8-bit is unsigned
                v[c] = ((int32_t)p[0] - 128) << 8;
            } else {                                   // 16-bit
                v[c] = s->be ? (int16_t)((p[0] << 8) | p[1])
                             : (int16_t)(p[0] | (p[1] << 8));
            }
        }
        int16_t l = (int16_t)v[0];
        int16_t r = (s->ch == 2) ? (int16_t)v[1] : l;
        dst[k * 2] = l;
        dst[k * 2 + 1] = r;
    }
    s->read += got;
    return got;
}

// ---- streaming cubic (Catmull-Rom) resampler to 44.1 kHz --------------------
typedef struct {
    double t;                  // fractional source position within the window
    double step;               // src frames per output frame
    int16_t h[4][2];           // last 4 source frames (window)
    int primed;                // frames in the window so far
} imp_rs_t;

static inline int16_t rs_cubic(const int16_t *y0, const int16_t *y1,
                               const int16_t *y2, const int16_t *y3,
                               int c, float x)
{
    float a = y0[c], b = y1[c], cc = y2[c], d = y3[c];
    float o = b + 0.5f * x * (cc - a +
              x * (2.0f * a - 5.0f * b + 4.0f * cc - d +
              x * (3.0f * (b - cc) + d - a)));
    if (o > 32767.0f) o = 32767.0f;
    if (o < -32768.0f) o = -32768.0f;
    return (int16_t)o;
}

// push src frames through the resampler; emits output frames via cb-less
// tight loop into out (cap out_max). Returns frames emitted; consumes all in.
static size_t rs_run(imp_rs_t *rs, const int16_t *in, size_t nin,
                     int16_t *out, size_t out_max)
{
    size_t emitted = 0;
    for (size_t k = 0; k < nin; k++) {
        // slide the window
        rs->h[0][0] = rs->h[1][0]; rs->h[0][1] = rs->h[1][1];
        rs->h[1][0] = rs->h[2][0]; rs->h[1][1] = rs->h[2][1];
        rs->h[2][0] = rs->h[3][0]; rs->h[2][1] = rs->h[3][1];
        rs->h[3][0] = in[k * 2];   rs->h[3][1] = in[k * 2 + 1];
        if (rs->primed < 4) {      // prime: replicate the first frame backward
            if (rs->primed == 0)
                for (int j = 0; j < 3; j++) {
                    rs->h[j][0] = in[0]; rs->h[j][1] = in[1];
                }
            rs->primed++;
            if (rs->primed < 4) continue;
        }
        // the window covers source positions [n-3 .. n]; interpolate between
        // h[1] and h[2] while the fractional position t is inside [0,1)
        while (rs->t < 1.0 && emitted < out_max) {
            float x = (float)rs->t;
            out[emitted * 2]     = rs_cubic(rs->h[0], rs->h[1], rs->h[2], rs->h[3], 0, x);
            out[emitted * 2 + 1] = rs_cubic(rs->h[0], rs->h[1], rs->h[2], rs->h[3], 1, x);
            emitted++;
            rs->t += rs->step;
        }
        rs->t -= 1.0;
    }
    return emitted;
}

// ---- one-file conversion -----------------------------------------------------
// a REAL MPEG frame header at p? Raw PCM fakes the 0xFF sync byte easily
// (bench: seven .RAW files got misrouted) — validate version/layer/bitrate/
// samplerate fields too.
static bool mp3_hdr_ok(const uint8_t *m)
{
    if (m[0] != 0xFF || (m[1] & 0xE0) != 0xE0) return false;
    if (((m[1] >> 3) & 0x3) == 1) return false;   // reserved MPEG version
    if (((m[1] >> 1) & 0x3) == 0) return false;   // reserved layer
    if ((m[2] >> 4) == 0xF) return false;         // invalid bitrate
    if (((m[2] >> 2) & 0x3) == 3) return false;   // invalid samplerate
    return true;
}

// frame length of a Layer III header (0 = not computable)
static size_t mp3_frame_len(const uint8_t *m)
{
    static const int br1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    static const int br2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
    static const int sr1[4]  = {44100,48000,32000,0};
    int ver = (m[1] >> 3) & 3;          // 3=MPEG1, 2=MPEG2, 0=MPEG2.5
    int bri = m[2] >> 4, sri = (m[2] >> 2) & 3, pad = (m[2] >> 1) & 1;
    int br = (ver == 3 ? br1 : br2)[bri];
    int sr = sr1[sri];
    if (ver == 2) sr /= 2;
    if (ver == 0) sr /= 4;
    if (!br || !sr) return 0;
    return (size_t)((ver == 3 ? 144 : 72) * br * 1000 / sr) + pad;
}

static bool magic_is_mp3(const char *path)
{
    // encoders pad the stream head (sox leads with 0x00; ID3v2 tags run KBs).
    // Discriminator vs raw PCM (which fakes single syncs easily): an ID3 tag,
    // or TWO valid frame headers CHAINED at the computed frame length.
    uint8_t buf[2048];
    size_t got;
    sd_lock_take();
    FILE *f = fopen(path, "rb");
    got = f ? fread(buf, 1, sizeof(buf), f) : 0;
    if (f) fclose(f);
    sd_lock_give();
    if (got < 4) return false;
    if (memcmp(buf, "ID3", 3) == 0) return true;
    for (size_t i = 0; i + 4 <= got; i++) {
        if (!mp3_hdr_ok(buf + i)) continue;
        size_t L1 = mp3_frame_len(buf + i);
        if (!L1 || i + L1 + 4 > got) continue;
        if (!mp3_hdr_ok(buf + i + L1)) continue;
        // THREE chained frames: loud clipped recordings are full of 0xFFFF
        // runs and one lucky offset two-chains (bench: 16 takes misrouted) —
        // a third exact hop is beyond luck
        size_t L2 = mp3_frame_len(buf + i + L1);
        if (!L2 || i + L1 + L2 + 4 > got) continue;
        if (mp3_hdr_ok(buf + i + L1 + L2)) return true;
    }
    return false;
}

static void vfs_to_fat(const char *vfs, char *fat, size_t n)
{
    const char *p = strncmp(vfs, "/sdcard/", 8) == 0 ? vfs + 8 : vfs;
    snprintf(fat, n, "%s", p);
}

// convert an open PCM description to <dst_vfs> (native WAV). 0 ok.
static int convert_pcm(imp_src_t *src, const char *dst_vfs)
{
    if (src->rate < 8000 || src->rate > 96000 || src->frames == 0) return -1;
    sd_lock_take();
    FILE *out = fopen(dst_vfs, "wb");
    if (out) sampwav_start(out);
    sd_lock_give();
    if (!out) return -1;

    uint8_t *raw = heap_caps_malloc(IMP_CHUNK * 8, MALLOC_CAP_DMA);   // <=8 B/frame
    int16_t *pcm = heap_caps_malloc(IMP_CHUNK * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    // output can exceed input frames when upsampling (worst x5.5 for 8k)
    size_t out_cap = IMP_CHUNK * 6 + 8;
    int16_t *res = heap_caps_malloc(out_cap * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int rc = -1;
    if (raw && pcm && res) {
        imp_rs_t rs = {0};
        rs.step = (double)src->rate / 44100.0;
        rc = 0;
        for (;;) {
            size_t got = src_read(src, pcm, IMP_CHUNK, raw);
            if (got == 0) break;
            size_t nout = (src->rate == 44100)
                ? got                              // fast path: no resample
                : rs_run(&rs, pcm, got, res, out_cap);
            const int16_t *w = (src->rate == 44100) ? pcm : res;
            sd_lock_take();
            size_t wr = fwrite(w, 4, nout, out);
            sd_lock_give();
            if (wr != nout) { rc = -1; break; }    // card full / IO error
            samp_import_pct = (int)((uint64_t)src->read * 100 / src->frames);
            vTaskDelay(1);                          // SD courtesy gap
        }
    }
    free(raw);
    if (pcm) heap_caps_free(pcm);
    if (res) heap_caps_free(res);
    sd_lock_take();
    if (rc == 0) sampwav_finish(out);
    fclose(out);
    if (rc != 0) remove(dst_vfs);
    sd_lock_give();
    return rc;
}

int samp_import_file(const char *vfs_path)
{
    strlcpy(samp_import_cur, strrchr(vfs_path, '/') ? strrchr(vfs_path, '/') + 1
                                                    : vfs_path,
            sizeof(samp_import_cur));
    samp_import_pct = 0;

    // destination: same directory, base name, .WAV
    char dst[96], tmp[96];
    strlcpy(dst, vfs_path, sizeof(dst));
    char *dot = strrchr(dst, '.');
    if (!dot) return -1;
    strcpy(dot, ".WAV");
    char *slash = strrchr(dst, '/');
    snprintf(tmp, sizeof(tmp), "%.*s/IMPNEW.TMP", (int)(slash - dst), dst);

    // MP3: helix -> temp interleaved s16 at native rate/ch, then convert that
    if (magic_is_mp3(vfs_path)) {
        char fin[96], ftmp[96];
        vfs_to_fat(vfs_path, fin, sizeof(fin));
        char *s2 = strrchr(fin, '/');
        snprintf(ftmp, sizeof(ftmp), "%.*s/IMPMP3.TMP", (int)(s2 - fin), fin);
        int ch = 0, rate = 0;
        if (decodeMP3FileSync(fin, ftmp, &ch, &rate, NULL, NULL) != 0 ||
            ch < 1 || ch > 2 || rate < 8000 || rate > 96000) {
            ESP_LOGE(TAG, "%s: mp3 decode failed", vfs_path);
            return -1;
        }
        char vtmp[112];
        snprintf(vtmp, sizeof(vtmp), "/sdcard/%.100s", ftmp);
        imp_src_t src = {0};
        sd_lock_take();
        src.f = fopen(vtmp, "rb");
        if (src.f) { fseek(src.f, 0, SEEK_END); long sz = ftell(src.f); fseek(src.f, 0, SEEK_SET);
                     src.frames = (uint32_t)(sz / (2 * ch)); }
        sd_lock_give();
        if (!src.f) return -1;
        src.rate = (uint32_t)rate; src.ch = (uint16_t)ch; src.bits = 16;
        src.code = 1; src.be = false;
        int rc = convert_pcm(&src, tmp);
        sd_lock_take();
        fclose(src.f);
        remove(vtmp);
        if (rc == 0) { remove(vfs_path); rename(tmp, dst); }
        sd_lock_give();
        if (rc == 0) ESP_LOGI(TAG, "%s -> %s (mp3 %d ch @%d)", vfs_path, dst, ch, rate);
        return rc;
    }

    // containers: probe; native passes untouched, convertible converts
    sampfile_t sf;
    sd_lock_take();
    FILE *f = fopen(vfs_path, "rb");
    sd_lock_give();
    if (!f) return -1;
    sd_lock_take();
    int pr = sampfile_probe(f, &sf);
    sd_lock_give();
    if (pr == 0) { sd_lock_take(); fclose(f); sd_lock_give(); return 1; }   // native
    bool convertible =
        sf.src_ch >= 1 && sf.src_ch <= 2 &&
        sf.src_rate >= 8000 && sf.src_rate <= 96000 &&
        ((sf.src_code == 1 && (sf.src_bits == 8 || sf.src_bits == 16 || sf.src_bits == 24)) ||
         (sf.src_code == 3 && sf.src_bits == 32));
    if (!convertible) {
        ESP_LOGE(TAG, "%s: not convertible (%s)", vfs_path, sf.why);
        sd_lock_take(); fclose(f); sd_lock_give();
        return -1;
    }
    imp_src_t src = {0};
    src.f = f;
    src.rate = sf.src_rate; src.ch = sf.src_ch; src.bits = sf.src_bits;
    src.code = sf.src_code;
    src.be = sf.be;                    // parser marks AIFF BE even on reject
    src.data_off = sf.src_data_off;
    src.frames = sf.src_data_len / ((sf.src_bits / 8) * sf.src_ch);
    sd_lock_take();
    fseek(f, src.data_off, SEEK_SET);
    sd_lock_give();
    int rc = convert_pcm(&src, tmp);
    sd_lock_take();
    fclose(f);
    if (rc == 0) { remove(vfs_path); rename(tmp, dst); }
    sd_lock_give();
    if (rc == 0) ESP_LOGI(TAG, "%s -> %s (%u-bit%s @%lu)", vfs_path, dst,
                          sf.src_bits, sf.src_code == 3 ? " float" : "",
                          (unsigned long)sf.src_rate);
    return rc;
}

// ---- pool scan ----------------------------------------------------------------
static bool ext_is(const char *name, const char *ext)
{
    int L = strlen(name), E = strlen(ext);
    return L > E && strcasecmp(name + L - E, ext) == 0;
}

static void import_task(void *pv)
{
    static const char *const dirs[] = {"/sdcard/usr", "/sdcard/usr/REC",
                                       "/sdcard/usr/LOOPS"};
    samp_import_done = 0;
    samp_import_fail = 0;
    samp_import_seen = 0;
    for (int di = 0; di < 3; di++) {
        sd_lock_take();
        DIR *d = opendir(dirs[di]);
        sd_lock_give();
        if (!d) continue;                  // folder may not exist yet (LOOPS)
        for (;;) {
            sd_lock_take();
            struct dirent *e = readdir(d);
            sd_lock_give();
            if (!e) break;
            // .RAW included: history (and pre-sniff uploads) left containers
            // behind .RAW names — the probe reads magic, so they convert too;
            // a genuine headerless RAW probes native and is skipped untouched
            bool cand = ext_is(e->d_name, ".MP3") || ext_is(e->d_name, ".WAV") ||
                        ext_is(e->d_name, ".AIF") || ext_is(e->d_name, ".AIFF") ||
                        ext_is(e->d_name, ".RAW");
            if (!cand || strncasecmp(e->d_name, "IMP", 3) == 0) continue;
            samp_import_seen++;
            char path[96];
            snprintf(path, sizeof(path), "%s/%.64s", dirs[di], e->d_name);
            int rc = samp_import_file(path);
            if (rc == 0) samp_import_done++;
            else if (rc < 0) samp_import_fail++;
            vTaskDelay(1);
        }
        sd_lock_take();
        closedir(d);
        sd_lock_give();
    }
    ESP_LOGI(TAG, "scan done: %d converted, %d failed",
             samp_import_done, samp_import_fail);
    samp_import_cur[0] = 0;
    samp_import_busy = false;
    vTaskDelete(NULL);
}

int samp_import_start(void)
{
    if (samp_import_busy) return -1;
    samp_import_busy = true;
    // unpinned, modest priority: it's a background chore
    // 20 KB: helix decodeMP3FileSync runs IN THIS TASK (the house MP3 tasks
    // give themselves 16 KB) plus the scan/convert frames on top — 12 KB
    // overflowed (bench: stack canary panic mid-scan)
    if (xTaskCreate(import_task, "importer", 20480, NULL, 5, NULL) != pdPASS) {
        samp_import_busy = false;
        return -1;
    }
    return 0;
}
