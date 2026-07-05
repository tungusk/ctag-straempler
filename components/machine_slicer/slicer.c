// M3 slicer engine — one stereo PSRAM sample, equal-grid slices, one
// monophonic retrigger voice. Runs in the audio task's process() callback;
// the UI task loads samples and pokes command flags (see slicer_priv.h).
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "machine.h"
#include "audio.h"
#include "slicer_priv.h"

sl_state_t sl;

// ---- slicing / playback ---------------------------------------------------
static uint32_t slice_len(void)
{
    if (sl.n_slices < 1 || sl.len == 0) return 0;
    uint32_t sl_ = sl.len / sl.n_slices;
    return sl_ < 4 ? sl.len : sl_;
}

static void fire_slice(int s)
{
    uint32_t sll = slice_len();
    if (sll == 0) return;
    if (s < 0) s = 0;
    if (s >= sl.n_slices) s = sl.n_slices - 1;
    sl.s_start = (uint32_t)s * sll;
    sl.s_end = sl.s_start + sll;
    if (sl.s_end > sl.len) sl.s_end = sl.len;
    sl.cur = s;
    sl.pos = sl.reverse ? (double)(sl.s_end - 1) : (double)sl.s_start;
    sl.playing = true;
}

static void end_slice(void)
{
    sl.playing = false;
    if (sl.auto_on)                      // walk to the next slice
        fire_slice((sl.cur + 1) % sl.n_slices);
}

// ---- lifecycle ------------------------------------------------------------
static esp_err_t slicer_start(void)
{
    memset(&sl, 0, sizeof(sl));
    sl.buf = heap_caps_malloc((size_t)SL_MAX_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!sl.buf) { ESP_LOGE("SLICER", "PSRAM alloc failed"); return ESP_ERR_NO_MEM; }
    sl.n_slices = 16;
    sl.level = 255;
    sl.pitch_cv = 2048;
    sl.inc = 1.0f;
    sl.loading = false;

    char first[1][24];
    if (slicer_list_samples(first, 1) > 0) slicer_load(first[0]);
    audio_status_set_voices("slicer", "");
    return ESP_OK;
}

static void slicer_stop(void)
{
    sl.playing = false;
    free(sl.buf);
    sl.buf = NULL;
}

// ---- audio ---------------------------------------------------------------
static void slicer_process(int32_t out[MACHINE_BLOCK],
                           const int32_t in[MACHINE_BLOCK],
                           const machine_io_t *io)
{
    (void)in;
    if (sl.loading || sl.len == 0 || !sl.buf) { memset(out, 0, MACHINE_BLOCK * sizeof(int32_t)); return; }

    // TR buttons (active low): TR1 = fire selected slice, TR2 = fire + step
    static uint8_t prev_trig = 0x03;
    uint8_t pressed = prev_trig & (~io->trig_level) & 0x03;
    prev_trig = io->trig_level;
    if (pressed & 1) sl.cmd_fire = 1;
    if (pressed & 2) sl.cmd_advance = 1;

    // knob 6 (CV6) selects the slice — update on knob movement so the encoder
    // still works when the knob is still (both write sl.sel, last mover wins)
    static uint16_t last_cv6 = 0xFFFF;
    uint16_t cv6 = io->cv[5];
    if (last_cv6 == 0xFFFF) last_cv6 = cv6;
    if (cv6 > last_cv6 + 40 || cv6 + 40 < last_cv6) {
        int s = (int)((uint32_t)cv6 * sl.n_slices / 4096);
        sl.sel = (s >= sl.n_slices) ? sl.n_slices - 1 : s;
        last_cv6 = cv6;
    }

    // CV1 jack = level (when driven, else unity — unpatched plays full volume)
    uint16_t c1 = io->cv[0] > 900 ? io->cv[0] - 900 : 0;   // 0..3195
    sl.level = c1 ? (uint16_t)((uint32_t)c1 * 255 / 3195) : 255;

    // knob 7 (CV7) pitch: a unity plateau around center (easy to hit 100%),
    // scaling to 0.5x .. 2.0x outside it
    sl.pitch_cv = io->cv[6];
    if (sl.pitch_cv >= 1843 && sl.pitch_cv <= 2253) sl.inc = 1.0f;         // ~±10% unity
    else if (sl.pitch_cv > 2253) sl.inc = 1.0f + (float)(sl.pitch_cv - 2253) / 1842.0f;
    else                         sl.inc = 0.5f + (float)sl.pitch_cv / 1843.0f * 0.5f;

    if (sl.cmd_fire)    { sl.cmd_fire = 0;    fire_slice(sl.sel); }
    if (sl.cmd_advance) { sl.cmd_advance = 0; fire_slice(sl.sel); sl.sel = (sl.sel + 1) % sl.n_slices; }
    if (sl.auto_on && !sl.playing) fire_slice(sl.cur);       // keep the auto sequence running

    int frames = MACHINE_BLOCK / 2;
    for (int f = 0; f < frames; f++) {
        int32_t l = 0, r = 0;
        if (sl.playing) {
            uint32_t i0 = (uint32_t)sl.pos;
            uint32_t i1 = i0 + 1;
            if (i1 >= sl.len) i1 = i0;
            float frac = (float)(sl.pos - (double)i0);
            int l0 = sl.buf[i0 * 2],     l1 = sl.buf[i1 * 2];
            int r0 = sl.buf[i0 * 2 + 1], r1 = sl.buf[i1 * 2 + 1];
            l = (l0 + (int)((l1 - l0) * frac)) * sl.level >> 8;
            r = (r0 + (int)((r1 - r0) * frac)) * sl.level >> 8;

            if (sl.reverse) {
                sl.pos -= sl.inc;
                if (sl.pos < (double)sl.s_start) end_slice();
            } else {
                sl.pos += sl.inc;
                if (sl.pos >= (double)sl.s_end) end_slice();
            }
        }
        out[f * 2]     = l << 16;
        out[f * 2 + 1] = r << 16;
    }
}

// ---- sample I/O (UI task) -------------------------------------------------
static void compute_peaks(void)
{
    for (int c = 0; c < SL_PEAKS; c++) {
        uint32_t a = (uint32_t)((uint64_t)c * sl.len / SL_PEAKS);
        uint32_t b = (uint32_t)((uint64_t)(c + 1) * sl.len / SL_PEAKS);
        int peak = 0;
        for (uint32_t i = a; i < b; i++) {
            int v = sl.buf[i * 2];     if (v < 0) v = -v;
            int w = sl.buf[i * 2 + 1]; if (w < 0) w = -w;
            if (v > peak) peak = v;
            if (w > peak) peak = w;
        }
        sl.peaks[c] = (uint8_t)((peak * 31) / 32768);
    }
    sl.peak_n = (sl.len > 0) ? SL_PEAKS : 0;
}

int slicer_load(const char *name)
{
    if (!sl.buf) return -1;
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/usr/%s.RAW", name);
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE("SLICER", "load: cannot open %s", path); return -1; }

    sl.loading = true;
    sl.playing = false;
    uint32_t n = 0;
    int32_t rbuf[256];
    size_t got;
    while (n < SL_MAX_FRAMES && (got = fread(rbuf, sizeof(int32_t), 256, f)) > 0) {
        for (size_t k = 0; k < got && n < SL_MAX_FRAMES; k++) {
            sl.buf[n * 2]     = (int16_t)(rbuf[k] & 0xFFFF);   // L
            sl.buf[n * 2 + 1] = (int16_t)(rbuf[k] >> 16);      // R
            n++;
        }
    }
    fclose(f);
    sl.len = n;
    strncpy(sl.sample, name, sizeof(sl.sample) - 1);
    sl.sample[sizeof(sl.sample) - 1] = 0;
    compute_peaks();
    sl.cur = 0;
    sl.sel = 0;
    sl.loading = false;
    ESP_LOGI("SLICER", "loaded %s (%lu frames)", name, (unsigned long)n);
    return 0;
}

int slicer_list_samples(char out[][24], int max)
{
    DIR *d = opendir("/sdcard/usr");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        int L = strlen(e->d_name);
        if (L > 4 && strcasecmp(e->d_name + L - 4, ".RAW") == 0) {
            int idl = L - 4;
            if (idl > 23) idl = 23;
            memcpy(out[n], e->d_name, idl);
            out[n][idl] = 0;
            n++;
        }
    }
    closedir(d);
    return n;
}

// persist settings + the loaded sample name so the slicer comes back the way
// you left it (reloads the remembered sample on bind)
static cJSON *slicer_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "slices", sl.n_slices);
    cJSON_AddStringToObject(o, "sample", sl.sample);
    cJSON_AddBoolToObject(o, "auto", sl.auto_on);
    cJSON_AddBoolToObject(o, "reverse", sl.reverse);
    return o;
}

static void slicer_preset_load(const cJSON *node)
{
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "slices")) && cJSON_IsNumber(j)) sl.n_slices = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "auto")))    sl.auto_on = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "reverse"))) sl.reverse = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sample")) && cJSON_IsString(j) && j->valuestring[0])
        slicer_load(j->valuestring);   // reload the remembered sample (SD ok on UI task)
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
