// M3 slicer engine — STREAMING edition. Per-slice attack heads in PSRAM, the
// playing slice's tail streamed from SD by a reader task (deck/sampler3
// discipline throughout: request flags, sd_lock per burst, process() never
// touches a file). See slicer_priv.h for the architecture note.
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sample_ram.h"
#include "sampfile.h"
#include "sd_lock.h"
#include "machine.h"
#include "audio.h"
#include "slicer_priv.h"

static const char *TAG = "SLICER";

sl_state_t sl;

static volatile bool s_run = false, s_alive = false;
#define SL_CHUNK 4096                    // reader chunk (frames); first is small

// ---- slicing (reader-task context: env lives in PSRAM) ---------------------
static void recompute_grid(int n)
{
    if (n < 1) n = 16;   // Auto has no meaning for an even grid — use 16
    if (n > SL_MAX_SLICES) n = SL_MAX_SLICES;
    sl.n_slices = n;
    for (int i = 0; i <= n; i++)
        sl.slice_pt[i] = (uint32_t)((uint64_t)i * sl.len / n);
}

#define SL_MAXCAND 192   // 2x the pool so 128 transient slices aren't starved

// target: 0 = Auto (keep every detected transient), else a max slice count.
static void pick_transients(int target)
{
    uint32_t nwin = sl.env_n;
    if (nwin < 4 || !sl.env) { recompute_grid(target); return; }
    float *env = sl.env;

    float maxo = 1.0f;
    for (uint32_t w = 2; w < nwin; w++) { float o = env[w] - env[w - 1]; if (o > maxo) maxo = o; }
    float thresh = maxo * (1.0f - (float)sl.sensitivity / 100.0f * 0.97f);
    if (thresh < maxo * 0.02f) thresh = maxo * 0.02f;
    uint32_t min_gap = (SL_RATE / 12) / SL_WIN;   // ~80 ms minimum slice
    if (min_gap < 1) min_gap = 1;

    float cs[SL_MAXCAND]; uint32_t cp[SL_MAXCAND]; int nc = 0;
    uint32_t last = 0; bool have_last = false;
    for (uint32_t w = 2; w < nwin - 1 && nc < SL_MAXCAND; w++) {
        float o = env[w] - env[w - 1];
        if (o > thresh && o >= (env[w - 1] - env[w - 2]) && o > (env[w + 1] - env[w])) {
            if (have_last && w - last < min_gap) continue;
            cs[nc] = o; cp[nc] = w * SL_WIN; nc++; last = w; have_last = true;
        }
    }
    if (nc == 0) { recompute_grid(target); return; }

    for (int i = 1; i < nc; i++) {   // sort candidates by strength desc
        float ks = cs[i]; uint32_t kp = cp[i]; int j = i - 1;
        while (j >= 0 && cs[j] < ks) { cs[j + 1] = cs[j]; cp[j + 1] = cp[j]; j--; }
        cs[j + 1] = ks; cp[j + 1] = kp;
    }
    int want = (target <= 0) ? nc : target - 1;   // Auto keeps all detected
    if (want > nc) want = nc;
    if (want > SL_MAX_SLICES - 1) want = SL_MAX_SLICES - 1;

    uint32_t pts[SL_MAX_SLICES];
    for (int i = 0; i < want; i++) pts[i] = cp[i];
    for (int i = 1; i < want; i++) {  // sort chosen positions ascending
        uint32_t k = pts[i]; int j = i - 1;
        while (j >= 0 && pts[j] > k) { pts[j + 1] = pts[j]; j--; }
        pts[j + 1] = k;
    }
    sl.slice_pt[0] = 0;
    for (int i = 0; i < want; i++) sl.slice_pt[i + 1] = pts[i];
    sl.slice_pt[want + 1] = sl.len;
    sl.n_slices = want + 1;
}

static void recompute_slices(void)
{
    if (sl.len == 0) { sl.n_slices = 1; sl.slice_pt[0] = 0; sl.slice_pt[1] = 0; return; }
    if (sl.ot_active && sl.ot_present && sl.ot_n > 0) {        // Octatrack sidecar
        memcpy(sl.slice_pt, sl.ot_pt, (size_t)(sl.ot_n + 1) * sizeof(uint32_t));
        sl.n_slices = sl.ot_n;
    }
    else if (sl.transient_mode) pick_transients(sl.slice_target);
    else                        recompute_grid(sl.slice_target);
    if (sl.sel >= sl.n_slices) sl.sel = sl.n_slices - 1;
}

// ---- reader task ------------------------------------------------------------
// read `n` PLAYBACK-ORDER frames of slice s starting at slice-relative p into
// dst — the sampler3 forward/reverse mapping, adapted to slice windows
static uint32_t read_slice_frames(FILE *f, const sampfile_t *sf, int s,
                                  uint32_t p, uint32_t n,
                                  int16_t *dst, int16_t *stage)
{
    uint32_t s0 = sl.slice_pt[s], s1 = sl.slice_pt[s + 1];
    if (s1 <= s0) return 0;
    uint32_t slen = s1 - s0;
    if (p >= slen) return 0;
    if (n > slen - p) n = slen - p;
    uint32_t got_total = 0;
    while (got_total < n) {
        uint32_t want = n - got_total;
        if (want > SL_CHUNK) want = SL_CHUNK;
        uint32_t pp = p + got_total;
        long file_frame = sl.reverse
            ? (long)s1 - 1 - (long)pp - (long)(want - 1)
            : (long)s0 + (long)pp;
        if (file_frame < 0) { want += file_frame; file_frame = 0; }
        sd_lock_take();
        fseek(f, sf_seek_pos(sf, (uint32_t)file_frame), SEEK_SET);
        size_t got = sampfile_read(f, sf, stage, want);
        sd_lock_give();
        if (got == 0) break;
        if (sl.reverse) {
            for (uint32_t i = 0; i < got; i++) {
                dst[(got_total + i) * 2]     = stage[(got - 1 - i) * 2];
                dst[(got_total + i) * 2 + 1] = stage[(got - 1 - i) * 2 + 1];
            }
        } else {
            memcpy(dst + got_total * 2, stage, got * 4);
        }
        got_total += got;
        if (got < want) break;
    }
    return got_total;
}

// one sequential pass: waveform peaks + transient envelope (chunked, paced)
static void scan_file(FILE *f, const sampfile_t *sf, int16_t *stage)
{
    memset(sl.peaks, 0, sizeof(sl.peaks));
    uint32_t nwin = sl.len / SL_WIN;
    if (nwin > SL_ENV_MAX - 2) nwin = SL_ENV_MAX - 2;   // detection cap ~10 min
    sl.env_n = 0;
    uint32_t frames_done = 0;
    uint64_t acc = 0;
    uint32_t win_fill = 0, win_i = 0;
    sd_lock_take();
    fseek(f, sf_seek_pos(sf, 0), SEEK_SET);
    sd_lock_give();
    while (frames_done < sl.len) {
        uint32_t want = sl.len - frames_done;
        if (want > SL_CHUNK) want = SL_CHUNK;
        sd_lock_take();
        size_t got = sampfile_read(f, sf, stage, want);
        sd_lock_give();
        if (got == 0) break;
        for (size_t k = 0; k < got; k++) {
            int l = stage[k * 2];     if (l < 0) l = -l;
            int r = stage[k * 2 + 1]; if (r < 0) r = -r;
            // peaks column
            int c = (int)((uint64_t)(frames_done + k) * SL_PEAKS / sl.len);
            if (c >= SL_PEAKS) c = SL_PEAKS - 1;
            int pk = (l > r ? l : r) * 31 / 32768;
            if (pk > sl.peaks[c]) sl.peaks[c] = (uint8_t)pk;
            // envelope window
            if (win_i < nwin) {
                acc += (uint32_t)(l + r);
                if (++win_fill == SL_WIN) {
                    sl.env[win_i++] = (float)acc;
                    acc = 0; win_fill = 0;
                }
            }
        }
        frames_done += got;
        vTaskDelay(1);                 // SD courtesy gap (house rule)
    }
    sl.env_n = win_i;
    sl.peak_n = (sl.len > 0) ? SL_PEAKS : 0;
}

static void build_heads(FILE *f, const sampfile_t *sf, int16_t *stage)
{
    sl.heads_valid = false;
    for (int i = 0; i < sl.n_slices && i < SL_MAX_SLICES; i++) {
        uint32_t slen = sl.slice_pt[i + 1] - sl.slice_pt[i];
        uint32_t hl = slen < SL_HEAD_FRAMES ? slen : SL_HEAD_FRAMES;
        int16_t *dst = sl.heads + (size_t)i * SL_HEAD_FRAMES * 2;
        uint32_t got = 0;
        while (got < hl) {
            uint32_t r = read_slice_frames(f, sf, i, got, hl - got,
                                           stage, stage);
            if (r == 0) break;
            memcpy(dst + got * 2, stage, r * 4);
            got += r;
        }
        sl.head_len[i] = got;
        vTaskDelay(1);                 // pace: 128 heads = 128 bursts
    }
    for (int i = sl.n_slices; i < SL_MAX_SLICES; i++) sl.head_len[i] = 0;
    sl.heads_valid = true;
}

static void reader_task(void *pv)
{
    FILE *f = NULL;
    sampfile_t sf = {0};
    bool was_reverse = sl.reverse;
    int16_t *stage = heap_caps_malloc(SL_CHUNK * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    uint32_t fill_gen = 0;             // generation the ring fill belongs to
    uint32_t wfill = 0;                // playback-order frames delivered
    uint32_t fill_slice_len = 0;
    s_alive = true;

    while (s_run) {
        if (sl.load_req) {
            sl.load_req = false;
            sl.loading = true;
            sl.playing = false;
            if (f) { sd_lock_take(); fclose(f); sd_lock_give(); f = NULL; }
            char path[80];
            sample_resolve(sl.pending, path, sizeof(path));
            sd_lock_take();
            f = fopen(path, "rb");
            if (f && sampfile_probe(f, &sf) != 0) {
                ESP_LOGE(TAG, "%s: %s", path, sf.why);
                fclose(f); f = NULL;
            }
            sd_lock_give();
            if (f) {
                sl.len = sf.frames;
                strlcpy(sl.sample, sl.pending, sizeof(sl.sample));
                scan_file(f, &sf, stage);
                int otn = slicer_parse_ot(sl.sample, sl.len, sl.ot_pt, SL_OT_SLICES + 1);
                sl.ot_n = otn > 0 ? otn : 0;
                sl.ot_present = sl.ot_active = (otn > 0);
                recompute_slices();
                build_heads(f, &sf, stage);
                sl.cur = 0;
                sl.sel = 0;
                ESP_LOGI(TAG, "loaded %s (%lu frames, %d slices, streaming)",
                         sl.sample, (unsigned long)sl.len, sl.n_slices);
            } else {
                sl.len = 0;
                sl.peak_n = 0;
                sl.env_n = 0;
            }
            fill_gen = sl.gen;         // any in-flight fill is stale now
            sl.ring_avail = 0;
            sl.loading = false;
        }

        if (f && (sl.resl_req || sl.reverse != was_reverse)) {
            sl.resl_req = false;
            was_reverse = sl.reverse;
            sl.loading = true;         // boundaries + heads are being rewritten
            sl.playing = false;
            recompute_slices();
            build_heads(f, &sf, stage);
            sl.ring_avail = 0;
            sl.loading = false;
        }

        // tail streaming: a new generation restarts the fill for its slice
        if (f && !sl.loading && sl.heads_valid) {
            uint32_t g = sl.gen;
            if (g != fill_gen || sl.ring_gen != g) {
                fill_gen = g;
                int s = sl.gen_slice;
                if (s >= 0 && s < sl.n_slices) {
                    fill_slice_len = sl.slice_pt[s + 1] - sl.slice_pt[s];
                    wfill = sl.head_len[s];
                    sl.ring_avail = wfill;     // ring valid from the head edge
                    sl.ring_gen = g;
                } else {
                    fill_slice_len = 0;
                }
            }
            if (sl.ring_gen == g && wfill < fill_slice_len) {
                uint32_t lead = wfill - sl.vpos_i;
                if ((int32_t)lead < (int32_t)(SL_RING_FRAMES - SL_CHUNK)) {
                    // FIRST chunk small: the ring must go live before a 2x-
                    // pitch voice exhausts the 80 ms head (~40 ms wall)
                    uint32_t want = (wfill == sl.head_len[sl.gen_slice]) ? 1024 : SL_CHUNK;
                    if (want > fill_slice_len - wfill) want = fill_slice_len - wfill;
                    uint32_t got = read_slice_frames(f, &sf, sl.gen_slice,
                                                     wfill, want, stage, stage);
                    if (got > 0) {
                        // ring is addressed by playback position % size
                        uint32_t w = wfill % SL_RING_FRAMES;
                        uint32_t first = SL_RING_FRAMES - w;
                        if (first > got) first = got;
                        memcpy(sl.ring + w * 2, stage, first * 4);
                        if (first < got)
                            memcpy(sl.ring, stage + first * 2, (got - first) * 4);
                        wfill += got;
                        sl.ring_avail = wfill;   // publish AFTER the copy
                        if (sl.gen == g) { vTaskDelay(1); continue; }
                    }
                }
            }
        }
        vTaskDelay(1);   // >=1 tick: shorter is a busy-spin (house rule)
    }
    if (f) { sd_lock_take(); fclose(f); sd_lock_give(); }
    free(stage);
    s_alive = false;
    vTaskDelete(NULL);
}

// ---- playback (audio task) --------------------------------------------------
static void fire_slice(int s)
{
    if (sl.len == 0 || sl.n_slices < 1 || !sl.heads_valid) return;
    if (s < 0) s = 0;
    if (s >= sl.n_slices) s = sl.n_slices - 1;
    sl.cur = s;
    sl.s_len = sl.slice_pt[s + 1] - sl.slice_pt[s];
    sl.pos = 0;
    sl.vpos_i = 0;
    sl.gen_slice = s;
    sl.gen++;                      // reader restarts the tail for this firing
    sl.playing = true;
}

static void end_slice(void)
{
    sl.playing = false;
    if (sl.auto_on)                // walk to the next slice
        fire_slice((sl.cur + 1) % sl.n_slices);
}

// one playback-order frame of the CURRENT firing: head first, then ring
static inline bool voice_frame(uint32_t p, int *l, int *r)
{
    if (p < sl.head_len[sl.cur]) {
        const int16_t *h = sl.heads + (size_t)sl.cur * SL_HEAD_FRAMES * 2;
        *l = h[p * 2]; *r = h[p * 2 + 1];
        return true;
    }
    if (sl.ring_gen == sl.gen && p < sl.ring_avail) {
        uint32_t i = p % SL_RING_FRAMES;
        *l = sl.ring[i * 2]; *r = sl.ring[i * 2 + 1];
        return true;
    }
    return false;                  // tail not delivered yet — starve
}

// ---- lifecycle ------------------------------------------------------------
static esp_err_t slicer_start(void)
{
    memset(&sl, 0, sizeof(sl));
    sl.heads = heap_caps_malloc((size_t)SL_MAX_SLICES * SL_HEAD_FRAMES * 2 * sizeof(int16_t),
                                MALLOC_CAP_SPIRAM);
    sl.ring  = heap_caps_malloc((size_t)SL_RING_FRAMES * 2 * sizeof(int16_t),
                                MALLOC_CAP_SPIRAM);
    sl.env   = heap_caps_malloc((size_t)SL_ENV_MAX * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!sl.heads || !sl.ring || !sl.env) {
        ESP_LOGE(TAG, "PSRAM alloc failed (heads %p ring %p env %p)",
                 sl.heads, sl.ring, sl.env);
        free(sl.heads); free(sl.ring); free(sl.env);
        sl.heads = NULL; sl.ring = NULL; sl.env = NULL;
        return ESP_ERR_NO_MEM;
    }
    sl.slice_target = 16;
    sl.n_slices = 16;
    sl.sensitivity = 50;
    sl.level = 255;
    sl.pitch_cv = 2048;
    sl.inc = 1.0f;
    s_run = true;
    // unpinned: file readers pinned to core 0 cause WiFi audio clicks
    xTaskCreate(reader_task, "sl_reader", 4096, NULL, 6, NULL);

    char first[1][24];
    if (slicer_list_samples(first, 1) > 0) slicer_load(first[0]);
    audio_status_set_voices("slicer", "");
    return ESP_OK;
}

static void slicer_stop(void)
{
    sl.playing = false;
    s_run = false;
    for (int i = 0; i < 100 && s_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    free(sl.heads); sl.heads = NULL;
    free(sl.ring);  sl.ring = NULL;
    free(sl.env);   sl.env = NULL;
}

// ---- audio ---------------------------------------------------------------
static void slicer_process(int32_t out[MACHINE_BLOCK],
                           const int32_t in[MACHINE_BLOCK],
                           const machine_io_t *io)
{
    (void)in;
    if (sl.loading || sl.len == 0 || !sl.heads) {
        memset(out, 0, MACHINE_BLOCK * sizeof(int32_t));
        return;
    }

    // TR buttons (active low): TR1 = fire selected slice, TR2 = fire + step
    static uint8_t prev_trig = 0x03;
    uint8_t pressed = prev_trig & (~io->trig_level) & 0x03;
    prev_trig = io->trig_level;
    if (pressed & 1) sl.cmd_fire = 1;
    if (pressed & 2) sl.cmd_advance = 1;

    // knob 6 (CV6) selects the slice — update on movement (encoder co-writes)
    static uint16_t last_cv6 = 0xFFFF;
    uint16_t cv6 = io->cv[5];
    if (last_cv6 == 0xFFFF) last_cv6 = cv6;
    if (cv6 > last_cv6 + 40 || cv6 + 40 < last_cv6) {
        int s = (int)((uint32_t)cv6 * sl.n_slices / 4096);
        sl.sel = (s >= sl.n_slices) ? sl.n_slices - 1 : s;
        last_cv6 = cv6;
    }

    // CV1 jack = level (when driven, else unity)
    uint16_t c1 = io->cv[0] > 900 ? io->cv[0] - 900 : 0;
    sl.level = c1 ? (uint16_t)((uint32_t)c1 * 255 / 3195) : 255;

    // knob 7 (CV7) pitch: unity plateau, 0.5x..2.0x outside it
    sl.pitch_cv = io->cv[6];
    if (sl.pitch_cv >= 1843 && sl.pitch_cv <= 2253) sl.inc = 1.0f;
    else if (sl.pitch_cv > 2253) sl.inc = 1.0f + (float)(sl.pitch_cv - 2253) / 1842.0f;
    else                         sl.inc = 0.5f + (float)sl.pitch_cv / 1843.0f * 0.5f;

    if (sl.cmd_fire)    { sl.cmd_fire = 0;    fire_slice(sl.sel); }
    if (sl.cmd_advance) { sl.cmd_advance = 0; fire_slice(sl.sel); sl.sel = (sl.sel + 1) % sl.n_slices; }
    if (sl.auto_on && !sl.playing && sl.heads_valid) fire_slice(sl.cur);

    int frames = MACHINE_BLOCK / 2;
    static float last_l = 0, last_r = 0;
    bool starved = false;
    for (int f = 0; f < frames; f++) {
        int32_t l = 0, r = 0;
        if (sl.playing) {
            uint32_t p0 = (uint32_t)sl.pos;
            if (p0 >= sl.s_len) { end_slice(); }
            if (sl.playing) {
                int l0, r0, l1, r1;
                uint32_t p1 = p0 + 1;
                if (p1 >= sl.s_len) p1 = p0;
                if (voice_frame(p0, &l0, &r0) && voice_frame(p1, &l1, &r1)) {
                    float frac = (float)(sl.pos - (double)p0);
                    l = (l0 + (int)((l1 - l0) * frac)) * sl.level >> 8;
                    r = (r0 + (int)((r1 - r0) * frac)) * sl.level >> 8;
                    last_l = l; last_r = r;
                    sl.pos += sl.inc;
                    sl.vpos_i = (uint32_t)sl.pos;
                } else {
                    // tail not delivered yet: hold position, decay the last
                    // sample out (no click), count the starve
                    starved = true;
                    last_l *= 0.94f; last_r *= 0.94f;
                    l = (int32_t)last_l; r = (int32_t)last_r;
                }
            }
        }
        out[f * 2]     = l << 16;
        out[f * 2 + 1] = r << 16;
    }
    if (starved) sl.dbg_starve++;
}

// ---- UI-side entry points ---------------------------------------------------
int slicer_load(const char *name)
{
    if (!sl.heads) return -1;
    strlcpy(sl.pending, name, sizeof(sl.pending));
    sl.loading = true;             // engine mutes until the reader finishes
    sl.playing = false;
    sl.load_req = true;            // reader does the I/O (async)
    return 0;
}

void slicer_reslice(void)
{
    sl.loading = true;
    sl.playing = false;
    sl.resl_req = true;            // reader recomputes + rebuilds heads
}

int slicer_list_samples(char out[][24], int max)
{
    return sample_list(out, max);
}

// persist settings + the loaded sample name
static cJSON *slicer_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "slices", sl.slice_target);
    cJSON_AddBoolToObject(o, "transient", sl.transient_mode);
    cJSON_AddNumberToObject(o, "sens", sl.sensitivity);
    cJSON_AddStringToObject(o, "sample", sl.sample);
    cJSON_AddBoolToObject(o, "auto", sl.auto_on);
    cJSON_AddBoolToObject(o, "reverse", sl.reverse);
    return o;
}

static void slicer_preset_load(const cJSON *node)
{
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "slices")) && cJSON_IsNumber(j)) sl.slice_target = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "transient"))) sl.transient_mode = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sens")) && cJSON_IsNumber(j)) sl.sensitivity = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "auto")))    sl.auto_on = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "reverse"))) sl.reverse = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sample")) && cJSON_IsString(j) && j->valuestring[0])
        slicer_load(j->valuestring);   // async: reader rebuilds everything
}

extern const machine_ui_t slicer_menu_ui;

const machine_t machine_slicer = {
    .name = "Slicer",
    .start = slicer_start,
    .stop = slicer_stop,
    .process = slicer_process,
    .preset_save = slicer_preset_save,
    .preset_load = slicer_preset_load,
    .ui = &slicer_menu_ui,
};
