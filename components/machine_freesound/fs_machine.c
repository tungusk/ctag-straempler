// Freesound machine — silent engine + the download → decode → install
// pipeline. All web/UI surfaces read the fsm state struct; the pipeline runs
// in its own (unpinned — it reads files) task, one job at a time.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
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
#include "sampimport.h"
#include "fs_auth.h"
#include "sampplay.h"
#include "sampfile.h"
#include "fs_priv.h"

static const char *TAG = "FSND-M";
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
// Largest body we will buffer in PSRAM before touching the card (see PHASE 1).
// A 90 s preview at 128 kbps is ~1.5 MB; 2 MB also stays under the single-alloc
// ceiling PSRAM starts refusing around.
#define FS_MEM_MP3_MAX (2 * 1024 * 1024)

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
// why the last fs_http_get failed, so callers can say something better than
// "unreachable" — which used to cover DNS, TLS, timeout, rate limiting and any
// non-200 alike, and is exactly the kind of catch-all that wastes an afternoon
static int  s_http_status = 0;      // HTTP status, 0 = never got a response
static esp_err_t s_http_err = ESP_OK;

int fs_http_status(void)   { return s_http_status; }
esp_err_t fs_http_err(void){ return s_http_err; }

int fs_http_get(const char *url, char **out, int max_len)
{
    *out = NULL;
    s_http_status = 0;
    s_http_err = ESP_OK;
    // IDF 4.3 esp-tls refuses https without a verification option — use the
    // built-in cert bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
    esp_http_client_config_t config = { .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 20000,
                                        .crt_bundle_attach = esp_crt_bundle_attach };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { s_http_err = ESP_ERR_NO_MEM; return -1; }
    int len = -1;
    esp_err_t oerr = esp_http_client_open(client, 0);
    s_http_err = oerr;
    if (oerr == ESP_OK) {
        stack_watch();
    int content_length = esp_http_client_fetch_headers(client);
        s_http_status = esp_http_client_get_status_code(client);
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

// Write the sidecar so the take lists like any other pool sample (/files only
// shows ids that have one). The audio itself is written by samp_import_file,
// which replaced this function's hand-rolled RAW copy and mono->stereo expand
// on 2026-09-13 — that copy could only handle 44.1 kHz material, which is what
// made most of freesound unusable.
static int fs_write_sidecar(const char *name, cJSON *meta, const char *id)
{
    char jsn_path[64];
    cJSON *sc = cJSON_CreateObject();
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.wav", name);
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

typedef struct { char id[16]; char name[SAMPLE_ID_LEN]; char url[320]; } fs_job_t;

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
        if (n <= 0) {
            char e[64];
            int st = fs_http_status();
            if (st > 0) snprintf(e, sizeof(e), "freesound HTTP %d", st);
            else        snprintf(e, sizeof(e), "no reply from freesound (%s)",
                                 esp_err_to_name(fs_http_err()));
            set_err(e);
            goto out;
        }
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
    // into the POOL dir, not a side cache: samp_import_file converts in place
    // and replaces the source, so the .WAV lands exactly where it belongs
    snprintf(pool_path, sizeof(pool_path), "/usr/%s.mp3", job->name);

    esp_http_client_config_t config = { .url = mp3_url, .method = HTTP_METHOD_GET, .timeout_ms = 20000,
                                        .crt_bundle_attach = esp_crt_bundle_attach };
    client = esp_http_client_init(&config);
    if (!client) { set_err("http init failed"); goto out; }
    if (esp_http_client_open(client, 0) != ESP_OK) { set_err("mp3 unreachable"); goto out; }
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length > FS_MAX_MP3_BYTES) { set_err("mp3 too large"); goto out; }
    // ---- PHASE 1: NETWORK ONLY -------------------------------------------
    // The card is not touched while the TLS session is live. Every SD failure
    // on the bench looked like this: the FIRST f_write after the handshake
    // returned FR_DISK_ERR (fr=1) because sdmmc_read_sectors_dma timed out
    // (0x107) on the FAT read, and once FatFS latches a disk error the retries
    // all fail instantly — so retrying was never going to work. Interleaving
    // 4 KB card writes with an active TLS download is the thing that breaks;
    // buffering the body and writing it afterwards separates the two.
    //
    // Previews are capped at 90 s, i.e. ~1.5 MB at 128 kbps, so this fits PSRAM
    // comfortably. A direct-URL fetch that is too big (or sends no length)
    // falls back to the old streaming path, which now at least fails cleanly.
    uint8_t *body = NULL;
    int total = 0, r;
    if (content_length > 0 && content_length <= FS_MEM_MP3_MAX)
        body = heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM);

    if (body) {
        while (total < content_length) {
            r = esp_http_client_read(client, (char *)body + total, content_length - total);
            if (r < 0) { heap_caps_free(body); set_err("mp3 read error"); goto out; }
            if (r == 0) break;
            total += r;
            fsm.progress = (int)((int64_t)total * 100 / content_length);
            stack_watch();
        }
        // network done — hand the radio back before going near the card
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;

        // ---- PHASE 2: CARD ONLY ------------------------------------------
        FRESULT fr;
        sd_lock_take();
        fr = f_open(&fmp3, pool_path, FA_CREATE_ALWAYS | FA_WRITE);
        sd_lock_give();
        if (fr != FR_OK) { heap_caps_free(body); set_err("SD open failed (pool)"); goto out; }
        fmp3_open = true;
        int off = 0;
        while (off < total) {
            UINT bw = 0;
            int want = total - off; if (want > 4096) want = 4096;
            sd_lock_take();
            fr = f_write(&fmp3, body + off, (UINT)want, &bw);
            sd_lock_give();
            if (fr != FR_OK || bw == 0) {
                ESP_LOGE(TAG, "SD write failed at %d/%d (fr=%d bw=%u)", off, total, fr, (unsigned)bw);
                heap_caps_free(body);
                set_err("SD write failed");
                goto out;
            }
            off += (int)bw;
        }
        heap_caps_free(body);
    } else {
        // streaming fallback: no content-length, or too big to buffer
        char *chunk = malloc(4096);
        if (!chunk) { set_err("out of memory"); goto out; }
        FRESULT fr;
        sd_lock_take();
        fr = f_open(&fmp3, pool_path, FA_CREATE_ALWAYS | FA_WRITE);
        sd_lock_give();
        if (fr != FR_OK) { free(chunk); set_err("SD open failed (pool)"); goto out; }
        fmp3_open = true;
        for (;;) {
            r = esp_http_client_read(client, chunk, 4096);
            if (r < 0) { free(chunk); set_err("mp3 read error"); goto out; }
            if (r == 0) break;
            UINT bw = 0;
            sd_lock_take();
            fr = f_write(&fmp3, chunk, (UINT)r, &bw);
            sd_lock_give();
            if (fr != FR_OK || bw != (UINT)r) { free(chunk); set_err("SD write failed"); goto out; }
            total += r;
            if (total > FS_MAX_MP3_BYTES) { free(chunk); set_err("mp3 too large"); goto out; }
            if (content_length > 0) fsm.progress = total / (content_length / 100 ? content_length / 100 : 1);
            stack_watch();
        }
        free(chunk);
    }
    sd_lock_take();
    f_close(&fmp3);
    sd_lock_give();
    if (total == 0) { set_err("empty mp3"); goto out; }

    // Release the HTTPS session if the streaming fallback still holds it: an
    // mbedtls context must not sit on internal RAM through the decode AND the
    // install, or install's buffers fail to allocate.
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
    }

    // --- convert in place, via the SAME path the Upload tab uses ---
    // This used to decode itself and then REJECT anything that was not already
    // 44.1 kHz — which is most of freesound, whose previews are largely 48 kHz,
    // so the machine refused a large share of its own library. util/sampimport
    // has decoded-and-resampled any rate from 8 to 96 kHz since 2026-07-13;
    // Freesound simply never called it. samp_import_file() replaces
    // usr/<name>.mp3 with a native 16-bit 44.1 kHz stereo usr/<name>.WAV,
    // handling the mono expand too, so fs_install's hand-rolled copy is gone.
    fsm.phase = FS_DECODE;
    fsm.progress = 0;
    stack_watch();
    ESP_LOGI(TAG, "pre-convert: internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    char vfs_mp3[96];
    snprintf(vfs_mp3, sizeof(vfs_mp3), "/sdcard%s", pool_path);
    if (samp_import_file(vfs_mp3) < 0) {
        sd_lock_take();
        f_unlink(pool_path);                 // don't leave an mp3 the pool can't play
        sd_lock_give();
        set_err("convert failed");
        goto out;
    }

    // --- sidecar, so the take lists like any other pool sample ---
    fsm.phase = FS_INSTALL;
    fsm.progress = 0;
    stack_watch();
    if (fs_write_sidecar(job->name, root, job->id) != 0) {
        set_err("sidecar write failed");
        goto out;
    }

    fsm.phase = FS_DONE;
    fsm.progress = 100;
    fsm.busy = false;
    ESP_LOGI(TAG, "installed %s as usr/%s.WAV",
             job->id[0] ? job->id : job->url, job->name);

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

// Library-safe take id: alnum/_/- only, up to SAMPLE_ID_LEN-1.
//
// This was 8 while the card was FatFS 8.3 with LFN off, where a 12-char stem
// made f_open return FR_INVALID_NAME and the install fail. LFN is on now
// (CONFIG_FATFS_LFN_HEAP), so a sound keeps a recognisable slice of its own
// title. The character filter stays strict on purpose: LFN permits more, but
// * ? < > | : " / \\ have no business in a pool id.
void fs_safe_name(const char *raw, const char *id, char *out, size_t n)
{
    size_t w = 0;
    if (raw)
        for (size_t i = 0; raw[i] && w < SAMPLE_ID_LEN - 1 && w + 1 < n; i++)
            if (isalnum((unsigned char)raw[i]) || raw[i] == '_' || raw[i] == '-')
                out[w++] = raw[i];
    out[w] = 0;
    if (!out[0] && id && id[0]) snprintf(out, n, "FS%s", id);
}

// ---- panel search ----------------------------------------------------------
// The web handler proxies raw JSON to the browser, which parses it there. The
// panel cannot, so this runs the same query and parses it HERE into results[].
// Own task, for the reason spelled out in fs_priv.h.
// MEASURED on .85: a real search left 19080 of 24576 free, i.e. a ~5.5 KB peak.
// TLS keeps its big buffers on the heap and the 40 KB body lives in PSRAM, so
// this is nothing like the download task's 28 KB. 12288 is 2.2x the measured
// peak — a wider margin than the pipeline's own 1.46x — and hands 12 KB of
// INTERNAL RAM back, which is the pool that decides whether a download can
// start at all. fsm.search_stack_min (in /fs/results) is the instrument; check
// it before trimming further.
#define FS_SEARCH_STACK 12288

static volatile bool s_searching = false;

static void search_stack_watch(void)
{
    unsigned free_bytes = (unsigned)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    if (fsm.search_stack_min == 0 || free_bytes < fsm.search_stack_min)
        fsm.search_stack_min = free_bytes;
}

const char *fs_search_err(void) { return fsm.serr; }

static void set_serr(const char *m)
{
    strlcpy(fsm.serr, m, sizeof(fsm.serr));
    fsm.search_state = FS_SEARCH_ERR;
    fsm.n_results = 0;
    ESP_LOGW(TAG, "search: %s", m);
}

// percent-encode a panel-typed query for the query string. The web path filters
// its already-encoded q instead; this one starts from raw text, so it has to do
// the encoding rather than the filtering.
static void url_escape(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < n; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.') out[o++] = (char)*p;
        else if (*p == ' ')                                     out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = 0;
}

typedef struct { char q[FS_QUERY_LEN]; int page; } fs_search_job_t;

static void fs_search_task(void *pv)
{
    fs_search_job_t *job = (fs_search_job_t *)pv;
    char *buf = NULL;
    cJSON *root = NULL;

    fsm.search_state = FS_SEARCH_RUNNING;
    fsm.n_results = 0;
    fsm.serr[0] = 0;
    search_stack_watch();
    wifiWaitForConnected();

    if (!fs_auth_ok()) { set_serr("no API key (System > Settings)"); goto out; }

    char esc[FS_QUERY_LEN * 3 + 4], auth[160], url[512];
    url_escape(job->q, esc, sizeof(esc));
    fs_auth_query_suffix(auth, sizeof(auth));
    snprintf(url, sizeof(url),
             "https://freesound.org/apiv2/search/text/?query=%s&page=%d&page_size=%d"
             "&fields=id,name,duration,username%s",
             esc, job->page, FS_RESULTS_MAX, auth);

    int n = fs_http_get(url, &buf, 48 * 1024);
    search_stack_watch();
    if (n <= 0) {
        char e[48];
        int st = fs_http_status();
        if (st == 429)      snprintf(e, sizeof(e), "rate limited by freesound (429)");
        else if (st == 401 || st == 403) snprintf(e, sizeof(e), "API key rejected (%d)", st);
        else if (st > 0)    snprintf(e, sizeof(e), "freesound HTTP %d", st);
        else                snprintf(e, sizeof(e), "no reply from freesound (%s)",
                                     esp_err_to_name(fs_http_err()));
        set_serr(e);
        goto out;
    }

    root = cJSON_Parse(buf);
    if (!root) { set_serr("bad JSON from freesound"); goto out; }
    cJSON *det = cJSON_GetObjectItem(root, "detail");
    if (det && cJSON_IsString(det)) { set_serr(det->valuestring); goto out; }

    cJSON *cnt = cJSON_GetObjectItem(root, "count");
    fsm.total = (cnt && cJSON_IsNumber(cnt)) ? cnt->valueint : 0;

    cJSON *res = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(res)) { set_serr("no results array"); goto out; }

    int k = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, res) {
        if (k >= FS_RESULTS_MAX) break;
        fs_result_t *r = &fsm.results[k];
        memset(r, 0, sizeof(*r));
        cJSON *j;
        j = cJSON_GetObjectItem(it, "id");
        if (j && cJSON_IsNumber(j)) snprintf(r->id, sizeof(r->id), "%d", j->valueint);
        j = cJSON_GetObjectItem(it, "name");
        if (j && cJSON_IsString(j)) strlcpy(r->name, j->valuestring, sizeof(r->name));
        j = cJSON_GetObjectItem(it, "username");
        if (j && cJSON_IsString(j)) strlcpy(r->user, j->valuestring, sizeof(r->user));
        j = cJSON_GetObjectItem(it, "duration");
        if (j && cJSON_IsNumber(j)) {
            double d = j->valuedouble * 10.0;
            r->dur_ds = (uint16_t)(d > 65535.0 ? 65535 : (d < 0 ? 0 : d));
        }
        if (r->id[0]) k++;                      // an entry with no id cannot be fetched
    }
    fsm.n_results = k;
    fsm.page = job->page;
    strlcpy(fsm.last_query, job->q, sizeof(fsm.last_query));
    fs_query_remember(job->q);
    fsm.search_state = FS_SEARCH_OK;
    ESP_LOGI(TAG, "search \"%s\" page %d -> %d/%d", job->q, job->page, k, fsm.total);

out:
    search_stack_watch();
    if (root) cJSON_Delete(root);
    if (buf) heap_caps_free(buf);
    free(job);
    s_searching = false;
    vTaskDelete(NULL);
}

int fs_search_start(const char *q, int page)
{
    if (s_searching || fsm.busy || !q || !q[0]) return -1;
    if (page < 1) page = 1;
    fs_search_job_t *job = calloc(1, sizeof(*job));
    if (!job) { set_serr("out of memory"); return -2; }
    strlcpy(job->q, q, sizeof(job->q));
    job->page = page;
    s_searching = true;
    fsm.search_stack_min = 0;
    fsm.search_state = FS_SEARCH_RUNNING;
    // unpinned, like the pipeline: this task touches the network, not audio
    if (xTaskCreate(fs_search_task, "fs_search", FS_SEARCH_STACK, job, 5, NULL) != pdPASS) {
        free(job);
        s_searching = false;
        set_serr("no RAM for the search task");
        return -2;
    }
    return 0;
}

// ---- query store -----------------------------------------------------------
void fs_query_remember(const char *q)
{
    if (!q || !q[0]) return;
    // already at the top? nothing to do
    if (fsm.n_recents > 0 && strcmp(fsm.recents[0], q) == 0) return;
    // drop an existing copy so the list stays de-duplicated
    for (int i = 0; i < fsm.n_recents; i++) {
        if (strcmp(fsm.recents[i], q) == 0) {
            for (int j = i; j < fsm.n_recents - 1; j++)
                strlcpy(fsm.recents[j], fsm.recents[j + 1], FS_QUERY_LEN);
            fsm.n_recents--;
            break;
        }
    }
    if (fsm.n_recents < FS_RECENTS) fsm.n_recents++;
    for (int i = fsm.n_recents - 1; i > 0; i--)
        strlcpy(fsm.recents[i], fsm.recents[i - 1], FS_QUERY_LEN);
    strlcpy(fsm.recents[0], q, FS_QUERY_LEN);
}

bool fs_query_is_saved(const char *q)
{
    if (!q || !q[0]) return false;
    for (int i = 0; i < fsm.n_saved; i++)
        if (strcmp(fsm.saved[i], q) == 0) return true;
    return false;
}

int fs_query_save(const char *q)
{
    if (!q || !q[0]) return -1;
    if (fs_query_is_saved(q)) return -2;
    if (fsm.n_saved >= FS_SAVED) return -1;
    strlcpy(fsm.saved[fsm.n_saved], q, FS_QUERY_LEN);
    fsm.n_saved++;
    return 0;
}

void fs_query_unsave(const char *q)
{
    for (int i = 0; i < fsm.n_saved; i++) {
        if (strcmp(fsm.saved[i], q) == 0) {
            for (int j = i; j < fsm.n_saved - 1; j++)
                strlcpy(fsm.saved[j], fsm.saved[j + 1], FS_QUERY_LEN);
            fsm.n_saved--;
            return;
        }
    }
}


// ---- audition ---------------------------------------------------------------
// The player is created ON FIRST AUDITION, not on machine start. Creating it
// eagerly cost a reader task out of INTERNAL RAM for the whole session, and
// internal RAM is the scarce pool here: it left the largest free block at
// 38912 against the download task's 40960, so every fetch failed with "no RAM
// for the download task" before a single sound could be heard. Searching and
// downloading now pay nothing for a player that is not playing.
static sampplay_t *s_play = NULL;
static char s_au_name[SAMPLE_ID_LEN];

int fs_audition(const char *name)
{
    if (!name || !name[0]) return -1;
    if (!s_play) s_play = sampplay_create(0);
    if (!s_play) return -1;
    if (sampplay_open(s_play, name) != 0) return -1;
    strlcpy(s_au_name, name, sizeof(s_au_name));
    sampplay_play(s_play, true);
    return 0;
}

void fs_audition_stop(void)
{
    if (!s_play) return;
    // process() renders s_play: take it away from the audio task first
    sampplay_t *p = s_play;
    s_play = NULL;
    machine_block_wait();
    sampplay_play(p, false);
    // tear the whole player down, not just the file: holding the reader task is
    // what starves the next download (see the note above)
    sampplay_destroy(p);
    s_au_name[0] = 0;
}

bool fs_auditioning(void) { return s_play && sampplay_playing(s_play); }
const char *fs_audition_name(void) { return s_au_name; }

// Drop the take we are auditioning. The pool lists ids that have a .JSN
// sidecar, so both have to go or the sample half-exists in the browser.
int fs_audition_drop(void)
{
    if (!s_au_name[0]) return -1;
    char name[SAMPLE_ID_LEN];
    strlcpy(name, s_au_name, sizeof(name));
    fs_audition_stop();

    // resolve BOTH paths before deleting anything: sample_resolve_aux() finds
    // the sidecar by locating the AUDIO file and swapping the extension, so
    // once the audio is gone it can no longer find the .JSN to remove
    char apath[80], jpath[80];
    sample_resolve(name, apath, sizeof(apath));
    bool have_jsn = (sample_resolve_aux(name, ".JSN", jpath, sizeof(jpath)) == 0);
    sd_lock_take();
    int r = remove(apath);
    if (have_jsn) remove(jpath);
    sd_lock_give();
    if (fsm.phase == FS_DONE) { fsm.phase = FS_IDLE; fsm.cur_name[0] = 0; }
    ESP_LOGI(TAG, "dropped %s (%d)", name, r);
    return r == 0 ? 0 : -1;
}

// ---- machine ---------------------------------------------------------------
static esp_err_t fsnd_start(void)
{
    if (!fsm.busy) {          // don't clobber a pipeline surviving a switch
        fsm.phase = FS_IDLE;
        fsm.progress = 0;
        fsm.err[0] = 0;
    }
    s_au_name[0] = 0;
    audio_status_set_voices("freesound", "");
    return ESP_OK;
}

static void fsnd_stop(void)
{
    if (s_play) { sampplay_destroy(s_play); s_play = NULL; }
    s_au_name[0] = 0;
    // nothing allocated; a running pipeline only touches static state and
    // finishes on its own
}

static void fsnd_process(int32_t out[MACHINE_BLOCK],
                         const int32_t in[MACHINE_BLOCK],
                         const machine_io_t *io)
{
    (void)in; (void)io;
    // Not a silent utility any more: a preview you cannot hear is not a preview.
    sampplay_render(s_play, out, MACHINE_BLOCK / 2, 1.0f);
}

static cJSON *fsnd_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "query", fsm.last_query);
    cJSON_AddBoolToObject(o, "autoplay", fsm.autoplay);
    // the query store is the whole point of the panel Live page — losing it on
    // a machine switch would put you back to typing every search
    cJSON *rc = cJSON_CreateArray();
    for (int i = 0; i < fsm.n_recents; i++)
        cJSON_AddItemToArray(rc, cJSON_CreateString(fsm.recents[i]));
    cJSON_AddItemToObject(o, "recents", rc);
    cJSON *sv = cJSON_CreateArray();
    for (int i = 0; i < fsm.n_saved; i++)
        cJSON_AddItemToArray(sv, cJSON_CreateString(fsm.saved[i]));
    cJSON_AddItemToObject(o, "saved", sv);
    return o;
}

static void load_str_array(const cJSON *node, const char *key,
                           char dst[][FS_QUERY_LEN], int max, int *n_out)
{
    *n_out = 0;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(node, key);
    if (!cJSON_IsArray(arr)) return;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (*n_out >= max) break;
        if (cJSON_IsString(it) && it->valuestring[0])
            strlcpy(dst[(*n_out)++], it->valuestring, FS_QUERY_LEN);
    }
}

static void fsnd_preset_load(const cJSON *node)
{
    if (!node) {
        // machine defaults: an empty store, not whatever the last machine left
        fsm.n_recents = 0;
        fsm.n_saved = 0;
        fsm.last_query[0] = 0;
        fsm.autoplay = true;
        return;
    }
    cJSON *j = cJSON_GetObjectItemCaseSensitive(node, "query");
    if (j && cJSON_IsString(j))
        strlcpy(fsm.last_query, j->valuestring, sizeof(fsm.last_query));
    cJSON *ap = cJSON_GetObjectItemCaseSensitive(node, "autoplay");
    fsm.autoplay = ap ? cJSON_IsTrue(ap) : true;
    load_str_array(node, "recents", fsm.recents, FS_RECENTS, &fsm.n_recents);
    load_str_array(node, "saved",   fsm.saved,   FS_SAVED,   &fsm.n_saved);
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
