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
#include "editor_priv.h"

static const char *TAG = "EDITOR";

ed_state_t ed;
const char *const ed_op_names[OP_N] = { "normalize", "reverse", "fade in", "fade out", "trim" };
// FatFS here is 8.3 ONLY (LFN off) -> every id must be <=8 chars. So outputs are
// short generated ids "<PFX>NNNN" (e.g. RV_0001), NOT <src>_<tag> which overflows.
static const char *const ED_PFX[OP_N] = { "NM_", "RV_", "FI_", "FO_", "TR_" };

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
    FILE *out = open_out(ed.op);
    if (!out) { set_err("output create failed"); goto close_src; }

    if (ed.op == OP_NORMALIZE) {
        // pass 1: peak
        int peak = 1;
        for (uint32_t p = 0; p < F; ) {
            size_t n = rd(src, &sf, buf, ED_CHUNK);
            if (n == 0) break;
            for (size_t i = 0; i < n * 2; i++) { int a = buf[i]; if (a < 0) a = -a; if (a > peak) peak = a; }
            p += n; ed.progress = (int)((uint64_t)p * 45 / F);
            vTaskDelay(1);
        }
        float gain = 0.99f * 32767.0f / (float)peak;
        if (gain > 32.0f) gain = 32.0f;      // don't blow up a near-silent file
        // pass 2: apply
        seek_frame(src, &sf, 0);
        for (uint32_t p = 0; p < F; ) {
            size_t n = rd(src, &sf, buf, ED_CHUNK);
            if (n == 0) break;
            for (size_t i = 0; i < n * 2; i++) buf[i] = clip16((float)buf[i] * gain);
            wr(out, buf, n);
            p += n; ed.progress = 50 + (int)((uint64_t)p * 50 / F);
            vTaskDelay(1);
        }
    } else if (ed.op == OP_REVERSE) {
        uint32_t pos = F;
        while (pos > 0) {
            uint32_t n = pos < ED_CHUNK ? pos : ED_CHUNK;
            uint32_t start = pos - n;
            seek_frame(src, &sf, start);
            size_t got = rd(src, &sf, buf, n);
            if (got == 0) break;
            for (uint32_t i = 0, j = (uint32_t)got - 1; i < j; i++, j--) {
                int16_t l = buf[i * 2], r = buf[i * 2 + 1];
                buf[i * 2] = buf[j * 2]; buf[i * 2 + 1] = buf[j * 2 + 1];
                buf[j * 2] = l; buf[j * 2 + 1] = r;
            }
            wr(out, buf, got);
            pos = start;
            ed.progress = (int)(100 - (uint64_t)pos * 100 / F);
            vTaskDelay(1);
        }
    } else if (ed.op == OP_FADEIN || ed.op == OP_FADEOUT) {
        float ms = ed.param > 0 ? ed.param : 50.0f;
        uint32_t fade = (uint32_t)(ms * ED_RATE / 1000.0f);
        if (fade > F) fade = F;
        uint32_t idx = 0;
        for (uint32_t p = 0; p < F; ) {
            size_t n = rd(src, &sf, buf, ED_CHUNK);
            if (n == 0) break;
            for (size_t i = 0; i < n; i++, idx++) {
                float g = 1.0f;
                if (ed.op == OP_FADEIN)  { if (idx < fade) g = (float)idx / (float)fade; }
                else                     { if (idx >= F - fade) g = (float)(F - 1 - idx) / (float)fade; }
                buf[i * 2]     = clip16((float)buf[i * 2] * g);
                buf[i * 2 + 1] = clip16((float)buf[i * 2 + 1] * g);
            }
            wr(out, buf, n);
            p += n; ed.progress = (int)((uint64_t)p * 100 / F);
            vTaskDelay(1);
        }
    } else if (ed.op == OP_TRIM) {
        int thr = ed.param > 0 ? (int)ed.param : 150;   // ~0.5% FS
        // pass 1: find first/last non-silent frame
        uint32_t first = F, last = 0, idx = 0;
        for (uint32_t p = 0; p < F; ) {
            size_t n = rd(src, &sf, buf, ED_CHUNK);
            if (n == 0) break;
            for (size_t i = 0; i < n; i++, idx++) {
                int a = buf[i * 2]; if (a < 0) a = -a;
                int b = buf[i * 2 + 1]; if (b < 0) b = -b;
                if (a > thr || b > thr) { if (idx < first) first = idx; last = idx; }
            }
            p += n; ed.progress = (int)((uint64_t)p * 45 / F);
            vTaskDelay(1);
        }
        if (first > last) { first = 0; last = F ? F - 1 : 0; }   // all silent -> keep whole
        // pass 2: copy [first, last]
        uint32_t span = last - first + 1;
        seek_frame(src, &sf, first);
        for (uint32_t p = 0; p < span; ) {
            uint32_t want = span - p; if (want > ED_CHUNK) want = ED_CHUNK;
            size_t n = rd(src, &sf, buf, want);
            if (n == 0) break;
            wr(out, buf, n);
            p += n; ed.progress = 50 + (int)((uint64_t)p * 50 / span);
            vTaskDelay(1);
        }
    }

    sd_lock_take();
    sampwav_finish(out);
    fclose(out);
    sd_lock_give();
    ed.progress = 100;
    ed.state = ED_DONE;
    ESP_LOGI(TAG, "%s: %s -> %s", ed_op_names[ed.op], ed.src, ed.out);

close_src:
    sd_lock_take();
    fclose(src);
    sd_lock_give();
done:
    free(buf);
    s_running = false;
    vTaskDelete(NULL);
}

void editor_apply(const char *src, int op, float param)
{
    if (s_running || !src || !src[0] || op < 0 || op >= OP_N) return;
    strlcpy(ed.src, src, sizeof(ed.src));
    ed.op = op;
    ed.param = param;
    ed.err[0] = 0;
    ed.out[0] = 0;
    ed.progress = 0;
    ed.state = ED_RUNNING;
    // helix-free, but sampfile + a 8 KB buffer on a modest stack; 8 KB is plenty
    if (xTaskCreate(job_task, "editor_job", 8192, NULL, 4, NULL) != pdPASS)
        set_err("job task create failed");
}

// ---- machine (silent) -------------------------------------------------------
static esp_err_t editor_start(void) { memset(&ed, 0, sizeof(ed)); ed.state = ED_IDLE; return ESP_OK; }
static void editor_stop(void)
{
    // let a running job finish/settle before we leave (it writes SD)
    for (int i = 0; i < 300 && s_running; i++) vTaskDelay(pdMS_TO_TICKS(10));
}
static void editor_process(int32_t out[MACHINE_BLOCK], const int32_t in[MACHINE_BLOCK], const machine_io_t *io)
{
    (void)in; (void)io;
    memset(out, 0, MACHINE_BLOCK * sizeof(int32_t));   // silent utility
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
