// Audio editor engine (see editor_priv.h). Each op streams a pool sample through
// a transform and writes a new derived take. Background task, sd_lock per burst,
// process() is silent. Reads via sampfile (int16 stereo), writes via sampwav.
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "machine.h"
#include "sampfile.h"
#include "sd_lock.h"
#include "sampplay.h"
#include "editor_priv.h"

static const char *TAG = "EDITOR";

ed_state_t ed;
const char *const ed_op_names[OP_N] = { "normalize", "reverse", "fade in", "fade out", "trim", "crop" };
// FatFS here is 8.3 ONLY (LFN off) -> every id must be <=8 chars. So outputs are
// short generated ids "<PFX>NNNN" (e.g. RV_0001), NOT <src>_<tag> which overflows.
static const char *const ED_PFX[OP_N] = { "NM_", "RV_", "FI_", "FO_", "TR_", "CR_" };

static volatile bool s_running = false;

static inline int16_t clip16(float v)
{
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)lrintf(v);
}

static void set_err(const char *m) { strlcpy(ed.err, m, sizeof(ed.err)); ed.state = ED_ERR; ESP_LOGW(TAG, "%s", m); }

// short 8.3-safe output id "<PFX>NNNN" (next free index for the op's prefix)
static void make_out_id(int op)
{
    int idx = sample_next_index(ED_PFX[op]);
    snprintf(ed.out, sizeof(ed.out), "%s%04d", ED_PFX[op], idx);
}

static FILE *open_src(sampfile_t *sf)
{
    char path[80];
    sample_resolve(ed.src, path, sizeof(path));
    sd_lock_take();
    FILE *f = fopen(path, "rb");
    if (f && sampfile_probe(f, sf) != 0) { fclose(f); f = NULL; }
    sd_lock_give();
    return f;
}

static FILE *open_out(int op)
{
    make_out_id(op);
    char path[80];
    snprintf(path, sizeof(path), "/sdcard/usr/%s.WAV", ed.out);
    sd_lock_take();
    FILE *f = fopen(path, "wb");
    if (f) sampwav_start(f);
    else ESP_LOGE(TAG, "output fopen %s failed: errno %d (%s)", path, errno, strerror(errno));
    sd_lock_give();
    return f;
}

static size_t rd(FILE *f, sampfile_t *sf, int16_t *buf, size_t n)
{
    sd_lock_take();
    size_t got = sampfile_read(f, sf, buf, n);
    sd_lock_give();
    return got;
}
static void wr(FILE *f, const int16_t *buf, size_t n)
{
    sd_lock_take();
    fwrite(buf, sizeof(int16_t) * 2, n, f);
    sd_lock_give();
}
static void seek_frame(FILE *f, const sampfile_t *sf, uint32_t frame)
{
    sd_lock_take();
    fseek(f, sf_seek_pos(sf, frame), SEEK_SET);
    sd_lock_give();
}

// ---- the transform ---------------------------------------------------------
// Every op except CROP writes the WHOLE file and transforms only [in_pt, out_pt).
// With in_pt = 0 and out_pt = frames that is bit-identical to the whole-file
// behaviour this machine shipped with, which is what keeps the old web calls
// honest.
typedef struct {
    int      op;
    uint32_t in, out, F;
    float    gain;          // normalize
    uint32_t fade;          // fade length in frames
} ed_xf_t;

static inline float frame_gain(const ed_xf_t *x, uint32_t idx)
{
    if (idx < x->in || idx >= x->out) return 1.0f;      // outside the range: untouched
    switch (x->op) {
        case OP_NORMALIZE: return x->gain;
        case OP_FADEIN: {
            uint32_t k = idx - x->in;
            return (x->fade && k < x->fade) ? (float)k / (float)x->fade : 1.0f;
        }
        case OP_FADEOUT: {
            uint32_t k = x->out - 1 - idx;
            return (x->fade && k < x->fade) ? (float)k / (float)x->fade : 1.0f;
        }
        default: return 1.0f;
    }
}

// copy [a, b) forward through frame_gain, reporting progress into [plo, phi]
static int span_fwd(FILE *src, sampfile_t *sf, FILE *out, int16_t *buf,
                    const ed_xf_t *x, uint32_t a, uint32_t b, int plo, int phi)
{
    if (b <= a) return 0;
    uint32_t span = b - a;
    seek_frame(src, sf, a);
    for (uint32_t p = 0; p < span; ) {
        uint32_t want = span - p;
        if (want > ED_CHUNK) want = ED_CHUNK;
        size_t n = rd(src, sf, buf, want);
        if (n == 0) break;
        for (size_t i = 0; i < n; i++) {
            float g = frame_gain(x, a + p + (uint32_t)i);
            if (g != 1.0f) {
                buf[i * 2]     = clip16((float)buf[i * 2]     * g);
                buf[i * 2 + 1] = clip16((float)buf[i * 2 + 1] * g);
            }
        }
        wr(out, buf, n);
        p += n;
        ed.progress = plo + (int)((uint64_t)p * (phi - plo) / span);
        vTaskDelay(1);
    }
    return 0;
}

// copy [a, b) BACKWARD (reverse), chunk-at-a-time from the tail
static int span_rev(FILE *src, sampfile_t *sf, FILE *out, int16_t *buf,
                    uint32_t a, uint32_t b, int plo, int phi)
{
    if (b <= a) return 0;
    uint32_t span = b - a, pos = b;
    while (pos > a) {
        uint32_t n = (pos - a) < ED_CHUNK ? (pos - a) : ED_CHUNK;
        uint32_t start = pos - n;
        seek_frame(src, sf, start);
        size_t got = rd(src, sf, buf, n);
        if (got == 0) break;
        for (uint32_t i = 0, j = (uint32_t)got - 1; i < j; i++, j--) {
            int16_t l = buf[i * 2], r = buf[i * 2 + 1];
            buf[i * 2] = buf[j * 2]; buf[i * 2 + 1] = buf[j * 2 + 1];
            buf[j * 2] = l; buf[j * 2 + 1] = r;
        }
        wr(out, buf, got);
        pos = start;
        ed.progress = plo + (int)((uint64_t)(b - pos) * (phi - plo) / span);
        vTaskDelay(1);
    }
    return 0;
}

static void job_task(void *pv)
{
    s_running = true;
    ed.progress = 0;
    ed.out[0] = 0;
    int16_t *buf = malloc((size_t)ED_CHUNK * 2 * sizeof(int16_t));
    sampfile_t sf = {0};
    FILE *src = buf ? open_src(&sf) : NULL;
    if (!buf || !src || sf.frames == 0) {
        set_err(buf ? "source open/probe failed" : "OOM");
        goto done;
    }
    uint32_t F = sf.frames;

    ed_xf_t x = { .op = ed.op, .F = F, .gain = 1.0f, .fade = 0 };
    x.in  = ed.in_pt < F ? ed.in_pt : 0;
    x.out = (ed.out_pt == 0 || ed.out_pt > F) ? F : ed.out_pt;
    if (x.out <= x.in) { x.in = 0; x.out = F; }     // a nonsense range means "all of it"

    FILE *out = open_out(ed.op);
    if (!out) { set_err("output create failed"); goto close_src; }

    if (ed.op == OP_NORMALIZE) {
        // pass 1: peak OVER THE RANGE ONLY — normalizing to a peak that lives
        // outside the selection would make the selection quieter, not louder
        int peak = 1;
        uint32_t span = x.out - x.in;
        seek_frame(src, &sf, x.in);
        for (uint32_t p = 0; p < span; ) {
            uint32_t want = span - p; if (want > ED_CHUNK) want = ED_CHUNK;
            size_t n = rd(src, &sf, buf, want);
            if (n == 0) break;
            for (size_t i = 0; i < n * 2; i++) { int a = buf[i]; if (a < 0) a = -a; if (a > peak) peak = a; }
            p += n; ed.progress = (int)((uint64_t)p * 45 / span);
            vTaskDelay(1);
        }
        x.gain = 0.99f * 32767.0f / (float)peak;
        if (x.gain > 32.0f) x.gain = 32.0f;      // don't blow up a near-silent file
        span_fwd(src, &sf, out, buf, &x, 0, F, 50, 100);

    } else if (ed.op == OP_REVERSE) {
        ed_xf_t flat = { .op = OP_N, .in = 0, .out = 0, .F = F, .gain = 1.0f };
        span_fwd(src, &sf, out, buf, &flat, 0, x.in, 0, 10);        // head, as-is
        span_rev(src, &sf, out, buf, x.in, x.out, 10, 90);          // the selection, flipped
        span_fwd(src, &sf, out, buf, &flat, x.out, F, 90, 100);     // tail, as-is

    } else if (ed.op == OP_FADEIN || ed.op == OP_FADEOUT) {
        float ms = ed.param > 0 ? ed.param : 50.0f;
        x.fade = (uint32_t)(ms * ED_RATE / 1000.0f);
        if (x.fade > x.out - x.in) x.fade = x.out - x.in;
        span_fwd(src, &sf, out, buf, &x, 0, F, 0, 100);

    } else if (ed.op == OP_CROP) {
        ed_xf_t flat = { .op = OP_N, .in = 0, .out = 0, .F = F, .gain = 1.0f };
        span_fwd(src, &sf, out, buf, &flat, x.in, x.out, 0, 100);   // the range, alone

    } else if (ed.op == OP_TRIM) {
        int thr = ed.param > 0 ? (int)ed.param : 150;   // ~0.5% FS
        // pass 1: find first/last non-silent frame INSIDE the range
        uint32_t span = x.out - x.in;
        uint32_t first = x.out, last = x.in, idx = x.in;
        seek_frame(src, &sf, x.in);
        for (uint32_t p = 0; p < span; ) {
            uint32_t want = span - p; if (want > ED_CHUNK) want = ED_CHUNK;
            size_t n = rd(src, &sf, buf, want);
            if (n == 0) break;
            for (size_t i = 0; i < n; i++, idx++) {
                int a = buf[i * 2]; if (a < 0) a = -a;
                int b = buf[i * 2 + 1]; if (b < 0) b = -b;
                if (a > thr || b > thr) { if (idx < first) first = idx; last = idx; }
            }
            p += n; ed.progress = (int)((uint64_t)p * 45 / span);
            vTaskDelay(1);
        }
        if (first > last) { first = x.in; last = x.out ? x.out - 1 : 0; }   // all silent -> keep it
        ed_xf_t flat = { .op = OP_N, .in = 0, .out = 0, .F = F, .gain = 1.0f };
        span_fwd(src, &sf, out, buf, &flat, first, last + 1, 50, 100);
    }

    sd_lock_take();
    sampwav_finish(out);
    fclose(out);
    sd_lock_give();
    ed.progress = 100;
    ed.state = ED_DONE;
    ESP_LOGI(TAG, "%s: %s [%u,%u) -> %s", ed_op_names[ed.op], ed.src,
             (unsigned)x.in, (unsigned)x.out, ed.out);

close_src:
    sd_lock_take();
    fclose(src);
    sd_lock_give();
done:
    free(buf);
    s_running = false;
    vTaskDelete(NULL);
}

void editor_apply(const char *src, int op, float param, uint32_t in, uint32_t out)
{
    if (s_running || !src || !src[0] || op < 0 || op >= OP_N) return;
    strlcpy(ed.src, src, sizeof(ed.src));
    ed.op = op;
    ed.param = param;
    ed.in_pt = in;
    ed.out_pt = out;
    ed.err[0] = 0;
    ed.out[0] = 0;
    ed.progress = 0;
    ed.state = ED_RUNNING;
    // helix-free, but sampfile + a 8 KB buffer on a modest stack; 8 KB is plenty
    if (xTaskCreate(job_task, "editor_job", 8192, NULL, 4, NULL) != pdPASS)
        set_err("job task create failed");
}

uint32_t editor_probe(const char *name)
{
    if (!name || !name[0]) return 0;
    char path[80];
    sample_resolve(name, path, sizeof(path));
    sd_lock_take();
    FILE *f = fopen(path, "rb");
    sampfile_t sf = {0};
    if (f && sampfile_probe(f, &sf) != 0) sf.frames = 0;
    if (f) fclose(f);
    sd_lock_give();
    return sf.frames;
}

// ---- source load + peak scan -----------------------------------------------
static volatile bool s_scanning = false;

static void scan_task(void *pv)
{
    (void)pv;
    s_scanning = true;
    ed.scanning = true;
    ed.scan_pct = 0;
    memset((void *)ed.peaks, 0, sizeof(ed.peaks));

    int16_t *buf = malloc((size_t)ED_CHUNK * 2 * sizeof(int16_t));
    sampfile_t sf = {0};
    FILE *f = buf ? open_src(&sf) : NULL;
    if (!buf || !f || sf.frames == 0) goto out;

    uint32_t F = sf.frames;
    for (uint32_t p = 0; p < F; ) {
        size_t n = rd(f, &sf, buf, ED_CHUNK);
        if (n == 0) break;
        for (size_t i = 0; i < n; i++) {
            int a = buf[i * 2];     if (a < 0) a = -a;
            int b = buf[i * 2 + 1]; if (b < 0) b = -b;
            if (b > a) a = b;
            int bin = (int)((uint64_t)(p + i) * ED_PEAKS / F);
            if (bin < 0) bin = 0;
            if (bin >= ED_PEAKS) bin = ED_PEAKS - 1;
            int v = a >> 7;                       // 0..32767 -> 0..255
            if (v > 255) v = 255;
            if (v > ed.peaks[bin]) ed.peaks[bin] = (uint8_t)v;
        }
        p += n;
        ed.scan_pct = (int)((uint64_t)p * 100 / F);
        vTaskDelay(1);                            // never starve the audio path
    }
    ed.scan_pct = 100;

out:
    if (f) { sd_lock_take(); fclose(f); sd_lock_give(); }
    free(buf);
    ed.scanning = false;
    s_scanning = false;
    vTaskDelete(NULL);
}

int editor_load(const char *name)
{
    if (s_running || s_scanning || !name || !name[0]) return -1;
    uint32_t F = editor_probe(name);
    if (F == 0) return -1;
    strlcpy(ed.src, name, sizeof(ed.src));
    ed.frames = F;
    ed.in_pt = 0;
    ed.out_pt = F;
    ed.state = ED_IDLE;
    ed.err[0] = 0;
    ed.out[0] = 0;
    if (xTaskCreate(scan_task, "editor_scan", 8192, NULL, 4, NULL) != pdPASS) return -1;
    return 0;
}

// ---- audition ---------------------------------------------------------------
static sampplay_t *s_play = NULL;

void editor_audition(bool on)
{
    if (!s_play) return;
    if (on) {
        if (!ed.src[0]) return;
        if (sampplay_open(s_play, ed.src) != 0) return;
        sampplay_window(s_play, ed.in_pt, ed.out_pt);
        sampplay_play(s_play, true);
    } else {
        sampplay_play(s_play, false);
        sampplay_close(s_play);
    }
}

bool editor_auditioning(void) { return s_play && sampplay_playing(s_play); }

// called after a crop move: the window changes under the player, which rewinds
void editor_audition_window(void)
{
    if (s_play && sampplay_playing(s_play)) sampplay_window(s_play, ed.in_pt, ed.out_pt);
}

uint32_t editor_play_pos(void) { return s_play ? sampplay_pos(s_play) : 0; }

// ---- clipboard + slice ------------------------------------------------------
// All three clipboard ops are STREAMING copies through a temp file, so they
// inherit the engine's any-length property. Tape's clipboard is a PSRAM bank
// and is capped by the tape; this one is capped by the card.
#define ED_CLIP_PATH "/sdcard/usr/EDCLIP.WAV"

enum { CJ_COPY = 0, CJ_CUT, CJ_PASTE, CJ_SLICE };
static volatile int s_cjob = -1;
static volatile int s_cjob_n = 0;

static FILE *open_path_w(const char *path)
{
    sd_lock_take();
    FILE *f = fopen(path, "wb");
    if (f) sampwav_start(f);
    sd_lock_give();
    return f;
}

// straight copy of [a,b) from src to out, no transform
static void raw_span(FILE *src, sampfile_t *sf, FILE *out, int16_t *buf,
                     uint32_t a, uint32_t b, int plo, int phi)
{
    if (b <= a) return;
    uint32_t span = b - a;
    seek_frame(src, sf, a);
    for (uint32_t p = 0; p < span; ) {
        uint32_t want = span - p; if (want > ED_CHUNK) want = ED_CHUNK;
        size_t n = rd(src, sf, buf, want);
        if (n == 0) break;
        wr(out, buf, n);
        p += n;
        ed.progress = plo + (int)((uint64_t)p * (phi - plo) / span);
        vTaskDelay(1);
    }
}

static void clip_task(void *pv)
{
    (void)pv;
    s_running = true;
    ed.progress = 0;
    ed.out[0] = 0;

    int16_t *buf = malloc((size_t)ED_CHUNK * 2 * sizeof(int16_t));
    sampfile_t sf = {0};
    FILE *src = buf ? open_src(&sf) : NULL;
    if (!buf || !src || sf.frames == 0) { set_err(buf ? "source open failed" : "OOM"); goto done; }
    uint32_t F = sf.frames;
    uint32_t a = ed.in_pt < F ? ed.in_pt : 0;
    uint32_t b = (ed.out_pt == 0 || ed.out_pt > F) ? F : ed.out_pt;
    if (b <= a) { a = 0; b = F; }

    if (s_cjob == CJ_COPY || s_cjob == CJ_CUT) {
        FILE *clip = open_path_w(ED_CLIP_PATH);
        if (!clip) { set_err("clipboard create failed"); goto close_src; }
        raw_span(src, &sf, clip, buf, a, b, 0, s_cjob == CJ_COPY ? 100 : 50);
        sd_lock_take(); sampwav_finish(clip); fclose(clip); sd_lock_give();
        ed.clip_frames = b - a;
        ed.clip_full = true;
    }

    if (s_cjob == CJ_CUT) {
        // a new take with the range REMOVED — the source is never touched
        FILE *out = open_out(OP_CROP);
        if (!out) { set_err("output create failed"); goto close_src; }
        raw_span(src, &sf, out, buf, 0, a, 50, 75);
        raw_span(src, &sf, out, buf, b, F, 75, 100);
        sd_lock_take(); sampwav_finish(out); fclose(out); sd_lock_give();
    } else if (s_cjob == CJ_PASTE) {
        if (!ed.clip_full) { set_err("clipboard is empty"); goto close_src; }
        sampfile_t cf = {0};
        sd_lock_take();
        FILE *clip = fopen(ED_CLIP_PATH, "rb");
        if (clip && sampfile_probe(clip, &cf) != 0) { fclose(clip); clip = NULL; }
        sd_lock_give();
        if (!clip) { set_err("clipboard open failed"); goto close_src; }
        FILE *out = open_out(OP_CROP);
        if (!out) { sd_lock_take(); fclose(clip); sd_lock_give(); set_err("output create failed"); goto close_src; }
        raw_span(src, &sf, out, buf, 0, a, 0, 35);
        raw_span(clip, &cf, out, buf, 0, cf.frames, 35, 70);
        raw_span(src, &sf, out, buf, a, F, 70, 100);
        sd_lock_take(); sampwav_finish(out); fclose(out); fclose(clip); sd_lock_give();
    } else if (s_cjob == CJ_SLICE) {
        // N equal slices of [a,b) into usr/SLICES, plus an .OT sidecar so the
        // Slicer machine can load the map instead of re-detecting it
        int n = s_cjob_n < 1 ? 1 : (s_cjob_n > 64 ? 64 : s_cjob_n);
        uint32_t span = (b - a) / (uint32_t)n;
        if (span < 64) { set_err("slices too short"); goto close_src; }
        for (int i = 0; i < n; i++) {
            int idx = sample_next_index("SLC_");
            char path[80];
            snprintf(path, sizeof(path), "/sdcard/usr/SLICES/SLC_%04d.WAV", idx);
            FILE *o = open_path_w(path);
            if (!o) { set_err("slice create failed"); goto close_src; }
            raw_span(src, &sf, o, buf, a + (uint32_t)i * span, a + (uint32_t)(i + 1) * span,
                     i * 100 / n, (i + 1) * 100 / n);
            sd_lock_take(); sampwav_finish(o); fclose(o); sd_lock_give();
            snprintf(ed.out, sizeof(ed.out), "SLC_%04d", idx);
        }
    }

    ed.progress = 100;
    ed.state = ED_DONE;

close_src:
    sd_lock_take();
    fclose(src);
    sd_lock_give();
done:
    free(buf);
    s_running = false;
    s_cjob = -1;
    vTaskDelete(NULL);
}

static int start_clip(int job, int n)
{
    if (s_running || s_scanning || !ed.src[0]) return -1;
    s_cjob = job;
    s_cjob_n = n;
    ed.err[0] = 0;
    ed.progress = 0;
    ed.state = ED_RUNNING;
    if (xTaskCreate(clip_task, "editor_clip", 8192, NULL, 4, NULL) != pdPASS) {
        s_cjob = -1;
        set_err("job task create failed");
        return -1;
    }
    return 0;
}

int editor_copy(void)        { return start_clip(CJ_COPY, 0); }
int editor_cut(void)         { return start_clip(CJ_CUT, 0); }
int editor_paste(void)       { return start_clip(CJ_PASTE, 0); }
int editor_slice(int n)      { return start_clip(CJ_SLICE, n); }

// ---- machine (silent) -------------------------------------------------------
static esp_err_t editor_start(void)
{
    memset(&ed, 0, sizeof(ed));
    ed.state = ED_IDLE;
    s_play = sampplay_create(0);          // ~1 s ring; NULL just means no audition
    return ESP_OK;
}
static void editor_stop(void)
{
    if (s_play) { sampplay_destroy(s_play); s_play = NULL; }
    // let a running job/scan finish/settle before we leave (they write SD)
    for (int i = 0; i < 300 && (s_running || s_scanning); i++) vTaskDelay(pdMS_TO_TICKS(10));
}
static void editor_process(int32_t out[MACHINE_BLOCK], const int32_t in[MACHINE_BLOCK], const machine_io_t *io)
{
    (void)in; (void)io;
    // Audible since 2026-09-12: the crop window loops while you drag it, which
    // is the difference between editing and guessing.
    sampplay_render(s_play, out, MACHINE_BLOCK / 2, 1.0f);
}
static cJSON *editor_preset_save(void) { return cJSON_CreateObject(); }
static void editor_preset_load(const cJSON *node) { (void)node; }

extern const machine_ui_t editor_menu_ui;

const machine_t machine_editor = {
    .name = "Editor",
    .start = editor_start,
    .stop = editor_stop,
    .process = editor_process,
    .preset_save = editor_preset_save,
    .preset_load = editor_preset_load,
    .ui = &editor_menu_ui,
};
