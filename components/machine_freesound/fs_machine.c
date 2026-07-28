// Freesound machine — silent engine + the download → decode → install
// pipeline. All web/UI surfaces read the fsm state struct; the pipeline runs
// in its own (unpinned — it reads files) task, one job at a time.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_vfs_fat.h"
#include "cJSON.h"
#include "machine.h"
#include "audio.h"
#include "wifi.h"
#include "fileio.h"
#include "sd_lock.h"
#include "mp3.h"
#include "fs_auth.h"
#include "fs_priv.h"

static const char *TAG = "FSND-M";
#define FS_TMP_RAW "/raw/FSTMP.RAW"
// THE DOWNLOAD USED TO PANIC THE WHOLE MODULE. This task was 8192*2 = 16384
// bytes and MEASURED PEAK USAGE IS 28168 — it overflowed its stack every time,
// took the device down with it, and the reboot made the failure invisible:
// /fs/state answered "idle" with an empty error because the machine had simply
// started over (2026-07-28).
//
// 40960 leaves ~31% headroom over the measured peak. The peak was identical to
// the byte across four different sounds, so it is dominated by fixed buffers
// (TLS session + the MP3 decoder), not by anything the sound controls — but a
// stack overflow costs a reboot, so the margin is deliberately generous.
// fs_state.stack_min reports the tightest free stack of the last run, so a
// change that adds appetite here shows up as a number instead of as a crash.
#define FS_PIPELINE_STACK (8192 * 5)

fs_state_t fsm;

const char *fs_phase_name(int phase)
{
    switch (phase) {
        case FS_DOWNLOAD: return "download";
        case FS_DECODE:   return "decode";
        case FS_INSTALL:  return "install";
        case FS_DONE:     return "done";
        case FS_ERROR:    return "error";
        default:          return "idle";
    }
}

// Record the tightest the pipeline stack has ever been. uxTaskGetStackHighWaterMark
// returns the minimum FREE stack in words; a task that reaches 0 does not report
// it, it dies, so this has to be sampled while things still work.
static void stack_watch(void)
{
    unsigned free_bytes = (unsigned)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    if (fsm.stack_min == 0 || free_bytes < fsm.stack_min) fsm.stack_min = free_bytes;
}

static void set_err(const char *msg)
{
    strlcpy(fsm.err, msg, sizeof(fsm.err));
    fsm.phase = FS_ERROR;
    fsm.busy = false;
    ESP_LOGE(TAG, "%s", msg);
}

// GET url into a fresh PSRAM buffer (NUL-terminated). Returns body length,
// -1 on error. Caller frees *out.
int fs_http_get(const char *url, char **out, int max_len)
{
    *out = NULL;
    // IDF 4.3 esp-tls refuses https without a verification option — use the
    // built-in cert bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
    esp_http_client_config_t config = { .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 20000,
                                        .crt_bundle_attach = esp_crt_bundle_attach };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return -1;
    int len = -1;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        stack_watch();
    int content_length = esp_http_client_fetch_headers(client);
        int cap = (content_length > 0 && content_length < max_len) ? content_length : max_len;
        char *buf = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM);
        if (buf) {
            int total = 0, r;
            while (total < cap) {
                r = esp_http_client_read(client, buf + total, (cap - total) > 512 ? 512 : (cap - total));
                if (r <= 0) break;
                total += r;
            }
            buf[total] = 0;
            *out = buf;
            len = total;
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return len;
}

static void decode_progress(int pct, void *arg)
{
    (void)arg;
    fsm.progress = pct;
}

// copy the decoded RAW into the usr/ library (mono → stereo expand) and write
// the sidecar so the sample shows up like any web-uploaded one
static int fs_install(const char *tmp_path, const char *name, int channels,
                      cJSON *meta, const char *id)
{
    char usr_path[48], jsn_path[64];
    snprintf(usr_path, sizeof(usr_path), "/usr/%s.RAW", name);

    FIL in, out;
    FRESULT fr;
    sd_lock_take();
    fr = f_open(&in, tmp_path, FA_READ);
    sd_lock_give();
    if (fr != FR_OK) return -1;
    sd_lock_take();
    fr = f_open(&out, usr_path, FA_CREATE_ALWAYS | FA_WRITE);
    sd_lock_give();
    if (fr != FR_OK) {
        sd_lock_take(); f_close(&in); sd_lock_give();
        return -1;
    }

    uint32_t sz = f_size(&in), done = 0;
    int16_t *ibuf = malloc(4096);                              // internal RAM
    int16_t *obuf = (channels == 1) ? malloc(8192) : NULL;
    int rc = (ibuf && (channels != 1 || obuf)) ? 0 : -1;
    while (rc == 0) {
        UINT nr = 0, bw = 0;
        sd_lock_take();
        f_read(&in, ibuf, 4096, &nr);
        sd_lock_give();
        if (nr == 0) break;
        if (channels == 1) {
            int n = nr / 2;
            for (int i = 0; i < n; i++) { obuf[i * 2] = ibuf[i]; obuf[i * 2 + 1] = ibuf[i]; }
            sd_lock_take(); f_write(&out, obuf, (UINT)n * 4, &bw); sd_lock_give();
        } else {
            sd_lock_take(); f_write(&out, ibuf, nr, &bw); sd_lock_give();
        }
        done += nr;
        if (sz) fsm.progress = (int)((uint64_t)done * 100 / sz);
    }
    free(ibuf);
    free(obuf);
    sd_lock_take();
    f_close(&in);
    f_close(&out);
    f_unlink(tmp_path);
    sd_lock_give();
    if (rc != 0) return rc;

    cJSON *sc = cJSON_CreateObject();
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.raw", name);
    cJSON_AddStringToObject(sc, "name", tmp);
    cJSON_AddStringToObject(sc, "id", name);
    cJSON *j = cJSON_GetObjectItem(meta, "name");
    cJSON_AddStringToObject(sc, "description", (j && cJSON_IsString(j)) ? j->valuestring : "");
    if (id[0]) snprintf(tmp, sizeof(tmp), "freesound %s", id);
    else       snprintf(tmp, sizeof(tmp), "mp3 import");
    cJSON_AddStringToObject(sc, "tags_s", tmp);
    j = cJSON_GetObjectItem(meta, "username");
    cJSON_AddStringToObject(sc, "username", (j && cJSON_IsString(j)) ? j->valuestring : "");
    j = cJSON_GetObjectItem(meta, "url");
    cJSON_AddStringToObject(sc, "url", (j && cJSON_IsString(j)) ? j->valuestring : "");
    j = cJSON_GetObjectItem(meta, "license");
    cJSON_AddStringToObject(sc, "license", (j && cJSON_IsString(j)) ? j->valuestring : "");
    snprintf(jsn_path, sizeof(jsn_path), "/sdcard/usr/%s.JSN", name);
    char *s = cJSON_Print(sc);
    cJSON_Delete(sc);
    if (s) { writeJSONFile(jsn_path, s); free(s); }
    return 0;
}

typedef struct { char id[16]; char name[24]; char url[320]; } fs_job_t;

static void fs_pipeline(void *pv)
{
    fs_job_t *job = (fs_job_t *)pv;
    char mp3_url[512], pool_path[48];
    char *buf = NULL;
    cJSON *root = NULL;
    esp_http_client_handle_t client = NULL;
    FIL fmp3;
    bool fmp3_open = false;

    fsm.phase = FS_DOWNLOAD;
    stack_watch();
    fsm.progress = 0;
    wifiWaitForConnected();

    if (job->url[0]) {
        // --- direct URL import: no metadata step, minimal sidecar info ---
        strlcpy(mp3_url, job->url, sizeof(mp3_url));
        root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", job->name);
        cJSON_AddStringToObject(root, "url", job->url);
    } else {
        // --- freesound: sound instance JSON → preview URL + metadata ---
        char url[512], auth[160];
        fs_auth_query_suffix(auth, sizeof(auth));
        snprintf(url, sizeof(url),
                 "https://freesound.org/apiv2/sounds/%s/?fields=id,name,username,license,url,duration,previews%s",
                 job->id, auth);
        int n = fs_http_get(url, &buf, 32768);
        if (n <= 0) { set_err("freesound unreachable"); goto out; }
        root = cJSON_Parse(buf);
        if (!root) { set_err("bad JSON from freesound"); goto out; }
        cJSON *det = cJSON_GetObjectItem(root, "detail");
        if (det && cJSON_IsString(det)) { set_err(det->valuestring); goto out; }
        cJSON *dur = cJSON_GetObjectItem(root, "duration");
        if (dur && cJSON_IsNumber(dur) && dur->valuedouble > FS_MAX_SECONDS) {
            set_err("sound too long (>90s preview)");
            goto out;
        }
        cJSON *previews = cJSON_GetObjectItem(root, "previews");
        cJSON *prev = previews ? cJSON_GetObjectItem(previews, "preview-hq-mp3") : NULL;
        if (!prev || !cJSON_IsString(prev)) { set_err("no preview url"); goto out; }
        strlcpy(mp3_url, prev->valuestring, sizeof(mp3_url));
    }

    // --- download the MP3 → /pool (kept as cache, like the classic browse) ---
    snprintf(pool_path, sizeof(pool_path), "/pool/%s.mp3", job->id[0] ? job->id : job->name);
    FRESULT fr;
    sd_lock_take();
    fr = f_open(&fmp3, pool_path, FA_CREATE_ALWAYS | FA_WRITE);
    sd_lock_give();
    if (fr != FR_OK) { set_err("SD open failed (pool)"); goto out; }
    fmp3_open = true;

    esp_http_client_config_t config = { .url = mp3_url, .method = HTTP_METHOD_GET, .timeout_ms = 20000,
                                        .crt_bundle_attach = esp_crt_bundle_attach };
    client = esp_http_client_init(&config);
    if (!client) { set_err("http init failed"); goto out; }
    if (esp_http_client_open(client, 0) != ESP_OK) { set_err("mp3 unreachable"); goto out; }
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length > FS_MAX_MP3_BYTES) { set_err("mp3 too large"); goto out; }
    char *chunk = malloc(4096);                    // internal RAM for the SD writes
    if (!chunk) { set_err("out of memory"); goto out; }
    int total = 0, r;
    for (;;) {
        r = esp_http_client_read(client, chunk, 4096);
        if (r < 0) { free(chunk); set_err("mp3 read error"); goto out; }
        if (r == 0) break;
        UINT bw;
        sd_lock_take();
        f_write(&fmp3, chunk, r, &bw);
        sd_lock_give();
        total += r;
        if (total > FS_MAX_MP3_BYTES) { free(chunk); set_err("mp3 too large"); goto out; }
        if (content_length > 0) fsm.progress = total / (content_length / 100 ? content_length / 100 : 1);
        stack_watch();
    }
    free(chunk);
    sd_lock_take();
    f_close(&fmp3);
    sd_lock_give();
    fmp3_open = false;
    if (total == 0) { set_err("empty mp3"); goto out; }

    // --- decode to a temp RAW (44.1 kHz only: the decoder does not resample) ---
    fsm.phase = FS_DECODE;
    fsm.progress = 0;
    stack_watch();
    int channels = 2, samprate = 44100;
    if (decodeMP3FileSync(pool_path, FS_TMP_RAW, &channels, &samprate, decode_progress, NULL) != 0) {
        set_err("decode failed");
        goto out;
    }
    if (samprate != 44100) {
        char e[48];
        snprintf(e, sizeof(e), "mp3 is %d Hz, need 44100", samprate);
        sd_lock_take();
        f_unlink(FS_TMP_RAW);
        sd_lock_give();
        set_err(e);
        goto out;
    }

    // --- install into the library ---
    fsm.phase = FS_INSTALL;
    fsm.progress = 0;
    stack_watch();
    if (fs_install(FS_TMP_RAW, job->name, channels, root, job->id) != 0) {
        set_err("install failed");
        goto out;
    }

    fsm.phase = FS_DONE;
    fsm.progress = 100;
    fsm.busy = false;
    ESP_LOGI(TAG, "installed %s as usr/%s (%s)",
             job->id[0] ? job->id : job->url, job->name,
             channels == 1 ? "mono>stereo" : "stereo");

out:
    stack_watch();
    if (fmp3_open) { sd_lock_take(); f_close(&fmp3); sd_lock_give(); }
    if (root) cJSON_Delete(root);
    if (buf) heap_caps_free(buf);
    if (client) esp_http_client_cleanup(client);
    free(job);
    vTaskDelete(NULL);
}

static int start_job(const char *id, const char *url, const char *name)
{
    if (fsm.busy) return -1;
    fsm.busy = true;
    fsm.phase = FS_DOWNLOAD;
    fsm.progress = 0;
    fsm.err[0] = 0;
    fsm.stack_min = 0;
    strlcpy(fsm.cur_id, id, sizeof(fsm.cur_id));
    strlcpy(fsm.cur_name, name, sizeof(fsm.cur_name));
    fs_job_t *job = calloc(1, sizeof(*job));
    if (!job) { fsm.busy = false; set_err("out of memory"); return -2; }
    strlcpy(job->id, id, sizeof(job->id));
    strlcpy(job->url, url, sizeof(job->url));
    strlcpy(job->name, name, sizeof(job->name));
    // unpinned: file-touching tasks pinned to core 0 cause WiFi audio clicks
    if (xTaskCreate(fs_pipeline, "fs_pipeline", FS_PIPELINE_STACK, job, 5, NULL) != pdPASS) {
        // 40 KB of INTERNAL RAM in one block. Reporting this as "busy" (which is
        // what a bare -1 becomes at the web layer) would send someone hunting for
        // a stuck job that does not exist.
        free(job);
        fsm.busy = false;
        set_err("no RAM for the download task");
        return -2;
    }
    return 0;
}

int fs_get_start(const char *id, const char *name)   { return start_job(id, "", name); }
int fs_fetch_start(const char *url, const char *name){ return start_job("", url, name); }

// ---- machine ---------------------------------------------------------------
static esp_err_t fsnd_start(void)
{
    if (!fsm.busy) {          // don't clobber a pipeline surviving a switch
        fsm.phase = FS_IDLE;
        fsm.progress = 0;
        fsm.err[0] = 0;
    }
    audio_status_set_voices("freesound", "");
    return ESP_OK;
}

static void fsnd_stop(void)
{
    // nothing allocated; a running pipeline only touches static state and
    // finishes on its own
}

static void fsnd_process(int32_t out[MACHINE_BLOCK],
                         const int32_t in[MACHINE_BLOCK],
                         const machine_io_t *io)
{
    (void)in; (void)io;
    memset(out, 0, MACHINE_BLOCK * sizeof(int32_t));   // silent utility machine
}

static cJSON *fsnd_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "query", fsm.last_query);
    return o;
}

static void fsnd_preset_load(const cJSON *node)
{
    if (!node) return;
    cJSON *j = cJSON_GetObjectItemCaseSensitive(node, "query");
    if (j && cJSON_IsString(j))
        strlcpy(fsm.last_query, j->valuestring, sizeof(fsm.last_query));
}

extern const machine_ui_t fs_menu_ui;

const machine_t machine_freesound = {
    .name = "Freesound",
    .start = fsnd_start,
    .stop = fsnd_stop,
    .process = fsnd_process,
    .preset_save = fsnd_preset_save,
    .preset_load = fsnd_preset_load,
    .ui = &fs_menu_ui,
};
