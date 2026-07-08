// Tracker engine — SD-loaded module playback via libxmp. The RENDER task owns
// the libxmp context and fills a PSRAM ring; process() only consumes the ring
// (no libxmp, no SD, no heap in the audio task). Mirrors the deck's producer/
// ring architecture and request-flag protocol.
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "machine.h"
#include "audio.h"
#include "sd_lock.h"
#include "xmp.h"
#include "tracker_priv.h"

static const char *TAG = "TRACKER";

trk_state_t trk;
const float trk_ppb[5] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
const char *const trk_ppb_names[5] = {"1 per 4 beats", "1 per 2 beats", "1 per beat", "2 per beat", "4 per beat"};

static volatile bool s_run = false, s_alive = false;

// module extensions libxmp can load (subset shown to the browser + accepted by
// upload). libxmp autodetects the real format from content; the extension is
// just the on-card naming + browser filter.
static const char *const TRK_EXTS[] = {
    ".MOD", ".XM", ".IT", ".S3M", ".669", ".MTM", ".OKT", ".ULT", ".FAR",
    ".MED", ".DBM", ".AMF", ".PTM", ".STM", ".DMF", ".GDM", ".IMF", ".LIQ",
};
#define TRK_N_EXTS (int)(sizeof(TRK_EXTS)/sizeof(TRK_EXTS[0]))

static bool has_mod_ext(const char *name)
{
    int L = strlen(name);
    for (int i = 0; i < TRK_N_EXTS; i++) {
        int el = strlen(TRK_EXTS[i]);
        if (L > el && strcasecmp(name + L - el, TRK_EXTS[i]) == 0) return true;
    }
    return false;
}

// ---- browser list (module files, full name incl. extension) -----------------
static char s_list[192][TRK_NAME_LEN];

static int cmp_name(const void *a, const void *b){ return strcasecmp((const char*)a, (const char*)b); }

int tracker_list_modules(char (**out)[TRK_NAME_LEN])
{
    sd_lock_take();
    DIR *d = opendir("/sdcard/usr");
    int n = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < 192) {
            if (!has_mod_ext(e->d_name)) continue;
            int L = strlen(e->d_name);
            if (L > TRK_NAME_LEN - 1) L = TRK_NAME_LEN - 1;
            memcpy(s_list[n], e->d_name, L);
            s_list[n][L] = 0;
            n++;
        }
        closedir(d);
    }
    sd_lock_give();
    qsort(s_list, n, sizeof(s_list[0]), cmp_name);
    *out = s_list;
    return n;
}

// ---- UI-side controls (set flags; render task acts) -------------------------
void tracker_request_load(const char *name)
{
    strlcpy(trk.file, name, sizeof(trk.file));
    strlcpy(trk.pending, name, sizeof(trk.pending));
    trk.playing = false;
    trk.loading = true;
    trk.state = TRK_LOADING;
    trk.load_req = true;
}

void tracker_toggle_play(void)
{
    if (trk.state != TRK_READY) return;
    if (trk.playing) { trk.playing = false; return; }
    trk.playing = true;
}

// ---- render task (sole libxmp caller) ---------------------------------------
static xmp_context s_ctx;
static bool s_have_module;
static int16_t s_scratch[TRK_CHUNK * 2];   // internal RAM staging (libxmp -> ring)

static void apply_sound_mode(void)
{
    // Amiga: no interpolation + wide stereo (classic tracker crunch).
    // Clean: cubic spline + gentler stereo blend.
    if (trk.amiga) {
        xmp_set_player(s_ctx, XMP_PLAYER_INTERP, XMP_INTERP_NEAREST);
        xmp_set_player(s_ctx, XMP_PLAYER_MIX, 70);
    } else {
        xmp_set_player(s_ctx, XMP_PLAYER_INTERP, XMP_INTERP_SPLINE);
        xmp_set_player(s_ctx, XMP_PLAYER_MIX, 30);
    }
    trk.sound_dirty = false;
}

// read the whole module file into a PSRAM buffer, chunked under sd_lock with
// the load-bearing per-chunk yield (full-stride SD reads starve the bus).
static void *load_file_psram(const char *name, long *out_size)
{
    char path[80];
    snprintf(path, sizeof(path), "/sdcard/usr/%s", name);
    sd_lock_take();
    FILE *f = fopen(path, "rb");
    long sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET); }
    sd_lock_give();
    if (!f) { strlcpy(trk.fail_why, "open failed", sizeof(trk.fail_why)); return NULL; }
    if (sz <= 0 || sz > TRK_MAX_FILE) {
        sd_lock_take(); fclose(f); sd_lock_give();
        strlcpy(trk.fail_why, sz > 0 ? "too big" : "empty", sizeof(trk.fail_why));
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    uint8_t *stage = malloc(16384);            // SDMMC DMA can't target PSRAM
    if (!buf || !stage) {
        sd_lock_take(); fclose(f); sd_lock_give();
        free(stage); if (buf) heap_caps_free(buf);
        strlcpy(trk.fail_why, "no memory", sizeof(trk.fail_why));
        return NULL;
    }
    long off = 0;
    while (off < sz) {
        long want = sz - off; if (want > 16384) want = 16384;
        sd_lock_take();
        size_t got = fread(stage, 1, want, f);
        sd_lock_give();
        if (got == 0) break;
        memcpy(buf + off, stage, got);
        off += got;
        vTaskDelay(1);                          // load-bearing SD-courtesy gap
    }
    free(stage);
    sd_lock_take(); fclose(f); sd_lock_give();
    if (off < sz) { heap_caps_free(buf); strlcpy(trk.fail_why, "read error", sizeof(trk.fail_why)); return NULL; }
    *out_size = sz;
    return buf;
}

static void do_load(void)
{
    trk.playing = false;
    if (s_have_module) { xmp_end_player(s_ctx); xmp_release_module(s_ctx); s_have_module = false; }
    trk.state = TRK_LOADING;
    trk.loading = true;

    long sz = 0;
    void *buf = load_file_psram(trk.pending, &sz);
    if (!buf) { trk.state = TRK_FAIL; return; }

    int r = xmp_load_module_from_memory(s_ctx, buf, sz);
    heap_caps_free(buf);                        // libxmp keeps its own copy
    if (r != 0) {
        snprintf(trk.fail_why, sizeof(trk.fail_why), "load err %d", r);
        trk.state = TRK_FAIL;
        return;
    }

    struct xmp_module_info mi;
    xmp_get_module_info(s_ctx, &mi);
    if (mi.mod && mi.mod->name[0]) strlcpy(trk.title, mi.mod->name, sizeof(trk.title));
    else strlcpy(trk.title, trk.pending, sizeof(trk.title));
    strlcpy(trk.fmt, mi.mod ? mi.mod->type : "?", sizeof(trk.fmt));
    trk.channels = mi.mod ? mi.mod->chn : 0;
    trk.num_pat  = mi.mod ? mi.mod->len : 0;
    trk.mod_bpm  = mi.mod ? mi.mod->bpm : 0;

    xmp_start_player(s_ctx, TRK_RATE, 0);
    apply_sound_mode();
    trk.tf_cur = 1.0f;

    trk.wpos = trk.rpos = 0;
    trk.loading = true;
    trk.state = TRK_READY;
    s_have_module = true;
    trk.playing = true;                         // autoplay on load (deck feel)
}

static void ring_flush(void)                    // drop buffered audio, refill
{
    trk.loading = true;
    vTaskDelay(pdMS_TO_TICKS(4));               // let process see loading, mute
    trk.wpos = trk.rpos = 0;
}

static void render_task(void *pv)
{
    s_ctx = xmp_create_context();
    s_have_module = false;
    s_alive = true;

    while (s_run) {
        if (trk.load_req) { trk.load_req = false; do_load(); continue; }

        if (s_have_module && trk.seek_req) {
            trk.seek_req = false;
            xmp_set_position(s_ctx, trk.seek_pos);
            ring_flush();
        }
        if (s_have_module && trk.restart_req) {
            trk.restart_req = false;
            xmp_restart_module(s_ctx);
            ring_flush();
        }
        if (s_have_module && trk.sound_dirty) apply_sound_mode();

        bool room = (trk.wpos - trk.rpos) < (TRK_RING_FRAMES - TRK_CHUNK);
        if (trk.playing && trk.state == TRK_READY && s_have_module && room) {
            // sync: rate-match the module tempo to the external clock via the
            // tempo factor (pitch-preserving). Slew so clock jitter drifts
            // instead of warbling. Nominal factor 1.0 when unsynced/unlocked.
            if (trk.sync && trk.clk.locked && trk.clk.bpm > 0 && trk.mod_bpm > 0) {
                float ext_beat = trk.clk.bpm / trk_ppb[trk.ppb_idx];
                float tgt = ext_beat / (float)trk.mod_bpm;   // module runs at mod_bpm; scale toward ext
                if (tgt < 0.5f) tgt = 0.5f;
                if (tgt > 2.0f) tgt = 2.0f;
                trk.tf_cur += 0.05f * (tgt - trk.tf_cur);
            } else {
                trk.tf_cur += 0.05f * (1.0f - trk.tf_cur);
            }
            xmp_set_tempo_factor(s_ctx, trk.tf_cur);

            int r = xmp_play_buffer(s_ctx, s_scratch, TRK_CHUNK * 4, trk.loop ? 0 : 1);
            if (r < 0) {                          // module ended (loop off)
                trk.playing = false;
            } else {
                uint32_t w = trk.wpos % TRK_RING_FRAMES;
                uint32_t first = TRK_RING_FRAMES - w;
                if (first > TRK_CHUNK) first = TRK_CHUNK;
                memcpy(trk.ring + w * 2, s_scratch, first * 4);
                if (first < TRK_CHUNK)
                    memcpy(trk.ring, s_scratch + first * 2, (TRK_CHUNK - first) * 4);
                trk.wpos += TRK_CHUNK;
            }
            struct xmp_frame_info fi;
            xmp_get_frame_info(s_ctx, &fi);
            trk.cur_pos = fi.pos; trk.cur_pat = fi.pattern; trk.cur_row = fi.row;
            trk.time_ms = fi.time; trk.total_ms = fi.total_time; trk.cur_bpm = fi.bpm;
            if (trk.wpos - trk.rpos >= TRK_LOW_WATER) trk.loading = false;
            continue;                             // keep filling, no delay
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (s_have_module) { xmp_end_player(s_ctx); xmp_release_module(s_ctx); }
    xmp_free_context(s_ctx);
    s_alive = false;
    vTaskDelete(NULL);
}

// ---- lifecycle --------------------------------------------------------------
static esp_err_t tracker_start(void)
{
    char keep_file[TRK_NAME_LEN];
    bool keep_loop = trk.loop, keep_sync = trk.sync, keep_amiga = trk.amiga;
    int keep_clk = trk.clk_src, keep_ppb = trk.ppb_idx;
    strlcpy(keep_file, trk.file, sizeof(keep_file));   // survive the memset

    memset(&trk, 0, sizeof(trk));
    trk.ring = heap_caps_malloc((size_t)TRK_RING_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!trk.ring) { ESP_LOGE(TAG, "PSRAM ring alloc failed"); return ESP_ERR_NO_MEM; }
    trk.loop = keep_loop; trk.sync = keep_sync; trk.amiga = keep_amiga;
    trk.clk_src = keep_clk; trk.ppb_idx = keep_ppb;
    strlcpy(trk.file, keep_file, sizeof(trk.file));
    if (trk.ppb_idx < 0 || trk.ppb_idx > 4) trk.ppb_idx = 4;
    trk.clk_base = 4095;
    trk.tf_cur = 1.0f;
    trk.state = TRK_EMPTY;
    clock_reset(&trk.clk);
    // widen the clock gate for multi-PPQN pulses (deck lesson): the default
    // 20..300 BPM per-pulse gate rejects every interval of a 4 PPQN clock.
    trk.clk.period_min = TRK_RATE * 60 / 300 / 4;
    trk.clk.period_max = TRK_RATE * 60 / 20;

    s_run = true;
    xTaskCreate(render_task, "trk_render", 8192, NULL, 5, NULL);   // unpinned
    audio_status_set_voices("tracker", "");

    if (trk.file[0]) tracker_request_load(trk.file);   // restore last module
    return ESP_OK;
}

static void tracker_stop(void)
{
    trk.playing = false;
    s_run = false;
    for (int i = 0; i < 200 && s_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    free(trk.ring);
    trk.ring = NULL;
}

static void tracker_process(int32_t out[MACHINE_BLOCK], const int32_t in[MACHINE_BLOCK],
                            const machine_io_t *io)
{
    if (!trk.ring) return;

    // transport gates: TR1 = restart at the top, TR2 = stop
    static uint8_t prev_trig = 0x03;
    uint8_t pressed = prev_trig & (~io->trig_level) & 0x03;
    prev_trig = io->trig_level;
    if (pressed & 1) { trk.restart_req = true; trk.playing = true; }
    if (pressed & 2) trk.playing = false;

    int frames = MACHINE_BLOCK / 2;
    static int16_t last_l = 0, last_r = 0;
    bool starved = false;
    for (int fno = 0; fno < frames; fno++) {
        bool can_play = trk.playing && !trk.loading && trk.rpos + 1 < trk.wpos;
        if (!can_play && trk.playing && !trk.loading && !trk.seek_req && trk.wpos > 0
            && trk.rpos + 1 >= trk.wpos)
            starved = true;
        float gt = can_play ? 1.0f : 0.0f;
        trk.out_gain += (gt - trk.out_gain) * 0.015f;
        if (!can_play) {
            last_l = (int16_t)(last_l * 0.94f);
            last_r = (int16_t)(last_r * 0.94f);
            out[fno * 2]     = (int32_t)last_l << 16;
            out[fno * 2 + 1] = (int32_t)last_r << 16;
            continue;
        }
        uint32_t i = trk.rpos % TRK_RING_FRAMES;
        int16_t l = (int16_t)(trk.ring[i * 2]     * trk.out_gain);
        int16_t r = (int16_t)(trk.ring[i * 2 + 1] * trk.out_gain);
        last_l = l; last_r = r;
        out[fno * 2]     = (int32_t)l << 16;
        out[fno * 2 + 1] = (int32_t)r << 16;
        trk.rpos++;
    }
    if (starved) trk.dbg_starve++;

    // CV clock conditioning (deck pattern): floor-tracked Schmitt on the clock
    // channel synthesises a clean square for the shared detector.
    uint16_t cv = io->cv[trk.clk_src];
    if ((int)cv < trk.clk_base) trk.clk_base = cv;               // dips instantly
    else trk.clk_base += ((int)cv - trk.clk_base) >> 9;          // rises slowly
    uint16_t sq;
    if (!trk.clk_high && (int)cv > trk.clk_base + 900) { trk.clk_high = true; }
    else if (trk.clk_high && (int)cv < trk.clk_base + 350) { trk.clk_high = false; }
    sq = trk.clk_high ? 4095 : 0;
    for (int f = 0; f < frames; f++) clock_tick(&trk.clk, sq);
}

// ---- preset -----------------------------------------------------------------
static cJSON *tracker_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "file", trk.file);
    cJSON_AddBoolToObject(o, "loop", trk.loop);
    cJSON_AddBoolToObject(o, "sync", trk.sync);
    cJSON_AddBoolToObject(o, "amiga", trk.amiga);
    cJSON_AddNumberToObject(o, "clk_src", trk.clk_src);
    cJSON_AddNumberToObject(o, "ppb", trk.ppb_idx);
    return o;
}

static void tracker_preset_load(const cJSON *node)
{
    // defaults first (also the NULL / other-machine-autosave path)
    trk.loop = true; trk.sync = false; trk.amiga = true;
    trk.clk_src = 7; trk.ppb_idx = 4; trk.file[0] = 0;
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "loop")))  trk.loop  = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sync")))  trk.sync  = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "amiga"))) trk.amiga = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "clk_src")) && cJSON_IsNumber(j)) trk.clk_src = j->valueint & 7;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "ppb")) && cJSON_IsNumber(j)) {
        trk.ppb_idx = j->valueint; if (trk.ppb_idx < 0) trk.ppb_idx = 0; if (trk.ppb_idx > 4) trk.ppb_idx = 4;
    }
    // file restore happens in start() (needs the render task up) — stash it
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "file")) && cJSON_IsString(j) && j->valuestring[0])
        strlcpy(trk.file, j->valuestring, sizeof(trk.file));
}

extern const machine_ui_t tracker_menu_ui;

const machine_t machine_tracker = {
    .name = "Tracker",
    .start = tracker_start,
    .stop = tracker_stop,
    .process = tracker_process,
    .preset_save = tracker_preset_save,
    .preset_load = tracker_preset_load,
    .ui = &tracker_menu_ui,
};
