#include "rest-api.h"
#include "index.html.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "ff.h"
#include <esp_http_server.h>
#include "ui_events.h"
#include "string_tools.h"
#include "fileio.h"
#include "sampfile.h"
#include "sampimport.h"
#include "sd_lock.h"
#include "disp_lock.h"
#include "tftspi.h"
#include "esp_heap_caps.h"
#include "audio.h"
#include "menu_config.h"
#include "machine.h"
#include "beatlisten.h"
#include "tuner.h"
#include "strampler_version.h"
#include "recording.h"
#include "wifi.h"
#include "freesound.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

static const char *TAG = "REST-API";
static httpd_handle_t server = NULL;
static xQueueHandle ui_ev_queue = NULL;
// teleremote gate — settings.remote in CONFIG.JSN (default on), toggled live
// from System→Settings via rest_remote_enable()
static int s_remote_on = 1;

void rest_remote_enable(int on)
{
    s_remote_on = on ? 1 : 0;
    ESP_LOGI(TAG, "teleremote %s", s_remote_on ? "enabled" : "disabled");
}

// ─── helpers ────────────────────────────────────────────────────────────────

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

// Extract query parameter value into buf[buflen].  Returns true on success.
static bool get_query_param(httpd_req_t *req, const char *key, char *buf, size_t buflen)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen < 2) return false;
    char *qs = malloc(qlen);
    if (!qs) return false;
    bool ok = false;
    if (httpd_req_get_url_query_str(req, qs, qlen) == ESP_OK)
        ok = (httpd_query_key_value(qs, key, buf, buflen) == ESP_OK);
    free(qs);
    return ok;
}

// ─── GET / (landing page) ────────────────────────────────────────────────────

static esp_err_t landing_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    // the page ships inside the firmware: after every flash the browser's
    // cached copy is stale, so tell it to revalidate every load
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, index_html, index_html_len);
    return ESP_OK;
}

// ─── sidecar bpm cache ───────────────────────────────────────────────────────
// PSRAM, keyed (id, dir, sidecar mtime+size): a rewritten .JSN moves the key,
// so entries self-invalidate — no hooks in upload/analyze/rename to maintain.
// Linear probe over a modest table; collisions just overwrite (it's a cache).
#define SCC_N 512
typedef struct {
    char     id[24];
    uint8_t  di;
    uint32_t mtime, size;
    float    bpm;
    bool     used;
} scc_ent_t;
static scc_ent_t *s_scc = NULL;

static int scc_slot(const char *id, int di)
{
    uint32_t h = 5381 ^ (uint32_t)di;
    for (const char *c = id; *c; c++) h = h * 33 + (uint8_t)*c;
    return (int)(h % SCC_N);
}

static bool sidecar_cache_get(const char *id, int di, uint32_t mtime,
                              uint32_t size, float *bpm)
{
    if (!s_scc) return false;
    scc_ent_t *e = &s_scc[scc_slot(id, di)];
    if (e->used && e->di == di && e->mtime == mtime && e->size == size &&
        strncmp(e->id, id, sizeof(e->id)) == 0) {
        *bpm = e->bpm;
        return true;
    }
    return false;
}

static void sidecar_cache_put(const char *id, int di, uint32_t mtime,
                              uint32_t size, float bpm)
{
    if (!s_scc) {
        s_scc = heap_caps_calloc(SCC_N, sizeof(scc_ent_t), MALLOC_CAP_SPIRAM);
        if (!s_scc) return;                    // no cache, no harm
    }
    scc_ent_t *e = &s_scc[scc_slot(id, di)];
    strlcpy(e->id, id, sizeof(e->id));
    e->di = (uint8_t)di;
    e->mtime = mtime;
    e->size = size;
    e->bpm = bpm;
    e->used = true;
}

// ─── GET /files ──────────────────────────────────────────────────────────────

static esp_err_t files_get_handler(httpd_req_t *req)
{
    // STREAMED response. Internal RAM on this board is tight enough that
    // building the whole file array + printing it in one shot OOMs (the list
    // came back empty). Instead we emit one small object per file as we walk
    // the directory, so peak memory stays flat no matter how many files exist.
    // Sidecars ARE read now (Arlo wants bpm + duration + newest-first), but under
    // the constraint that made them forbidden before: the old attempt built cJSON
    // per file into PSRAM, and SDMMC DMA cannot target PSRAM — every read failed and
    // the handler dragged until sockets reset. So: ONE small INTERNAL-RAM buffer,
    // reused per file, and a substring parse for the two numbers we want. No cJSON,
    // no allocation, flat memory, and the socket-abort path is untouched.
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    // the opening brace rides in the output batcher below (obuf)

    // KEEP IN SYNC with SAMPLE_DIR_* (sample_ram.h) and the other pool-folder
    // arrays in this file (fdirs, del_dirs, mv_dirs/mv_tags, rn_dirs).
    static const char *const jdirs[] = {"/sdcard/usr", "/sdcard/usr/REC",
                                        "/sdcard/usr/LOOPS", "/sdcard/usr/SLICES",
                                        "/sdcard/usr/DRUMS", "/sdcard/usr/KEYS",
                                        "/sdcard/usr/TAPE"};
    // short folder tag streamed with each entry so the browser can SHOW the
    // card's organization instead of flattening it (pool/takes/loops/slices)
    static const char *const jdir_tag[] = {"pool", "REC", "LOOPS", "SLICES", "DRUMS", "KEYS", "TAPE"};
    #define JN_DIRS 7
    // INTERNAL RAM, reused for every sidecar. Never PSRAM: SDMMC DMA cannot target it,
    // which is the whole reason sidecars were skipped here in the first place.
    static char sidecar_buf[512];
    // output batcher (internal RAM, reused per request — one client at a time)
    static char obuf[3072];
    int obuf_len = 0;
    obuf_len = snprintf(obuf, sizeof(obuf), "{\"files\":[");
    bool first = true;
    bool dead = false;   // client hung up mid-stream — stop sending (same
                         // disease as the /files/raw abort fix: a dead socket
                         // must not keep the worker pumping chunks at it)

    // ONE raw-FatFS pass per folder. stat()-by-name is a LINEAR scan of the
    // FAT directory, so per-entry stats made the walk O(n^2) — measured
    // ~72 ms/entry, ~18-20 s for 254 files, and neither the bpm cache nor
    // send batching moved it. f_readdir hands back name+size+timestamp in
    // the same pass (the sample_list_recent lesson); the only per-entry
    // opens left are sidecar READS on bpm-cache misses. mtime is now the
    // FAT-packed date<<16|time — a sort key, which is all the web uses.
    typedef struct {
        char     id[24];
        uint32_t asize;          // audio container size (0 = none seen)
        uint32_t amt;            // audio FAT date<<16|time
        uint32_t jmt, jsz;       // sidecar stamp+size (bpm cache key)
        uint8_t  jsn, ot;
    } fidx_t;
    #define FIDX_MAX 512
    static fidx_t *idx = NULL;   // PSRAM, one folder at a time (~22 KB)
    if (!idx) idx = heap_caps_malloc(FIDX_MAX * sizeof(fidx_t), MALLOC_CAP_SPIRAM);
    static const char *const fdirs[] = {"usr", "usr/REC", "usr/LOOPS", "usr/SLICES", "usr/DRUMS", "usr/KEYS", "usr/TAPE"};

    for (int di = 0; idx && di < JN_DIRS && !dead; di++) {
        int n_idx = 0;
        FF_DIR d;
        // hold the bus for the WHOLE name-only scan (the sample_list rule):
        // per-dirent take/give was ~500 lock cycles + SD re-seeks and left the
        // walk at ~3 s; a held sequential dir read is ~100 ms per folder
        sd_lock_take();
        FRESULT fr = f_opendir(&d, fdirs[di]);
        if (fr != FR_OK) { sd_lock_give(); continue; }
        for (;;) {
            FILINFO fi;
            FRESULT r = f_readdir(&d, &fi);
            if (r != FR_OK || !fi.fname[0]) break;
            if (fi.fattrib & AM_DIR) continue;
            char *dot = strrchr(fi.fname, '.');
            if (!dot || dot == fi.fname) continue;
            size_t sl = (size_t)(dot - fi.fname);
            if (sl >= sizeof(((fidx_t *)0)->id)) continue;
            int cls;                              // 0 audio, 1 sidecar, 2 slice map
            if (!strcasecmp(dot, ".RAW") || !strcasecmp(dot, ".WAV") ||
                !strcasecmp(dot, ".AIF") || !strcasecmp(dot, ".AIFF")) cls = 0;
            else if (!strcasecmp(dot, ".JSN")) cls = 1;
            else if (!strcasecmp(dot, ".OT")) cls = 2;
            else continue;
            // find-or-add by stem (RAM-only linear probe; a folder tops out
            // at a few hundred entries — microseconds, not SD time)
            int k = -1;
            for (int i = 0; i < n_idx; i++)
                if (!strncasecmp(idx[i].id, fi.fname, sl) && idx[i].id[sl] == 0) { k = i; break; }
            if (k < 0) {
                if (n_idx >= FIDX_MAX) continue;
                k = n_idx++;
                memset(&idx[k], 0, sizeof(idx[k]));
                memcpy(idx[k].id, fi.fname, sl);
            }
            uint32_t w = ((uint32_t)fi.fdate << 16) | fi.ftime;
            if (cls == 0) { if (!idx[k].asize) { idx[k].asize = fi.fsize; idx[k].amt = w; } }
            else if (cls == 1) { idx[k].jsn = 1; idx[k].jmt = w; idx[k].jsz = fi.fsize; }
            else idx[k].ot = 1;
        }
        f_closedir(&d);
        sd_lock_give();

        for (int i = 0; i < n_idx && !dead; i++) {
            if (!idx[i].jsn) continue;   // a .JSN marks a complete sample
            // bpm behind the PSRAM cache keyed on the sidecar's stamp+size:
            // a rewritten .JSN moves the key and self-invalidates
            float bpm = 0.0f;
            if (!sidecar_cache_get(idx[i].id, di, idx[i].jmt, idx[i].jsz, &bpm)) {
                char jp[320];
                snprintf(jp, sizeof(jp), "%s/%s.JSN", jdirs[di], idx[i].id);
                sd_lock_take();
                FILE *jf = fopen(jp, "rb");
                if (jf) {
                    size_t got = fread(sidecar_buf, 1, sizeof(sidecar_buf) - 1, jf);
                    fclose(jf);
                    sidecar_buf[got] = 0;
                    const char *b = strstr(sidecar_buf, "\"bpm\"");
                    if (b) { b = strchr(b, ':'); if (b) bpm = strtof(b + 1, NULL); }
                }
                sd_lock_give();
                sidecar_cache_put(idx[i].id, di, idx[i].jmt, idx[i].jsz, bpm);
            }
            // duration: stereo 16-bit at 44.1k is the pool's native shape, so
            // bytes/4 is frames — a browsing aid, not a timeline
            int dur = idx[i].asize ? (int)(idx[i].asize / (4UL * 44100UL)) : 0;

            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", idx[i].id);
            cJSON_AddStringToObject(o, "dir", jdir_tag[di]);
            cJSON_AddStringToObject(o, "description", "");
            cJSON_AddStringToObject(o, "tags", "");
            cJSON_AddNumberToObject(o, "size", idx[i].asize);
            cJSON_AddNumberToObject(o, "bpm", bpm);
            cJSON_AddNumberToObject(o, "dur", dur);
            cJSON_AddNumberToObject(o, "mtime", idx[i].amt);
            cJSON_AddNumberToObject(o, "ot", idx[i].ot);
            char *os = cJSON_PrintUnformatted(o);
            cJSON_Delete(o);
            if (os) {
                // batched output: ~500 per-entry chunked sends were ~500 tiny
                // TCP writes — fill a buffer, flush in ~3 KB chunks
                int ol = strlen(os);
                if (obuf_len + ol + 2 > (int)sizeof(obuf) - 4) {
                    if (httpd_resp_send_chunk(req, obuf, obuf_len) != ESP_OK) dead = true;
                    obuf_len = 0;
                }
                if (!dead) {
                    if (!first) obuf[obuf_len++] = ',';
                    memcpy(obuf + obuf_len, os, ol);
                    obuf_len += ol;
                }
                free(os);
                first = false;
            }
        }
    }

    if (dead) return ESP_FAIL;             // socket gone — no trailer to send
    memcpy(obuf + obuf_len, "]}", 2);      // room reserved by the flush margin
    obuf_len += 2;
    httpd_resp_send_chunk(req, obuf, obuf_len);
    httpd_resp_send_chunk(req, NULL, 0);   // end of stream
    return ESP_OK;
}

// ─── DELETE /files?name=xxx ───────────────────────────────────────────────────

static esp_err_t files_delete_handler(httpd_req_t *req)
{
    char name[32];
    if (!get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }

    char path[72];
    static const char *const del_exts[] = {".RAW", ".WAV", ".AIF", ".AIFF", ".JSN", ".OT"};
    // SLICES + DRUMS were historically absent here, so a web delete couldn't
    // reach a file living in those folders; added, and the bound now derives
    // from the array so it can't drift again.
    static const char *const del_dirs[] = {"/sdcard/usr", "/sdcard/usr/REC",
                                           "/sdcard/usr/LOOPS", "/sdcard/usr/SLICES",
                                           "/sdcard/usr/DRUMS", "/sdcard/usr/KEYS",
                                           "/sdcard/usr/TAPE"};
    sd_lock_take();
    for (int d = 0; d < (int)(sizeof(del_dirs)/sizeof(del_dirs[0])); d++)
        for (int i = 0; i < 6; i++) {
            snprintf(path, sizeof(path), "%s/%s%s", del_dirs[d], name, del_exts[i]);
            remove(path);
        }
    sd_lock_give();

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

// ─── POST /files/move?name=xxx&dir=pool|REC|LOOPS ────────────────────────────
// Folder organization from the web: the same all-pieces sweep as rename (audio
// + .JSN + .OT travel together), destination = another pool folder, id kept.
static const char *const mv_dirs[] = {"/sdcard/usr", "/sdcard/usr/REC",
                                      "/sdcard/usr/LOOPS", "/sdcard/usr/SLICES",
                                      "/sdcard/usr/DRUMS", "/sdcard/usr/KEYS",
                                      "/sdcard/usr/TAPE"};
static const char *const mv_tags[] = {"pool", "REC", "LOOPS", "SLICES", "DRUMS", "KEYS", "TAPE"};
#define MV_DIRS 7

// sweep every piece of a sample (audio + .JSN + .OT, wherever each lives)
// into mv_dirs[dst]. Creates the folder on first use; never clobbers a twin.
// Returns pieces moved (0 = nothing found / already all there).
static int files_move_pieces(const char *name, int dst)
{
    static const char *const mv_exts[] = {".RAW", ".WAV", ".AIF", ".AIFF", ".JSN", ".OT"};
    char from_p[80], to_p[80];
    int moved = 0;
    sd_lock_take();
    mkdir(mv_dirs[dst], 0775);                         // idempotent
    for (int d = 0; d < MV_DIRS; d++) {
        if (d == dst) continue;
        for (int i = 0; i < 6; i++) {
            snprintf(from_p, sizeof(from_p), "%s/%s%s", mv_dirs[d], name, mv_exts[i]);
            struct stat st;
            if (stat(from_p, &st) != 0) continue;      // this piece isn't here
            snprintf(to_p, sizeof(to_p), "%s/%s%s", mv_dirs[dst], name, mv_exts[i]);
            struct stat dt;
            if (stat(to_p, &dt) == 0) continue;        // never clobber a twin
            if (rename(from_p, to_p) == 0) moved++;
        }
    }
    sd_lock_give();
    return moved;
}

static esp_err_t files_move_handler(httpd_req_t *req)
{
    char name[32], dir[12];
    if (!get_query_param(req, "name", name, sizeof(name)) ||
        !get_query_param(req, "dir", dir, sizeof(dir))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name/dir");
        return ESP_FAIL;
    }
    int dst = -1;
    for (int d = 0; d < MV_DIRS; d++)
        if (strcasecmp(dir, mv_tags[d]) == 0) dst = d;
    if (dst < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "dir must be pool/REC/LOOPS/SLICES/DRUMS/KEYS/TAPE");
        return ESP_FAIL;
    }
    if (!files_move_pieces(name, dst)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such sample (or already there)");
        return ESP_FAIL;
    }
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// ─── POST /files/rename?name=xxx&to=yyy ───────────────────────────────────────
//
// A pool id is not just a filename: it is an audio file, a .JSN sidecar (carrying the
// bpm/grid stamp the deck and DoubleDecker depend on), and possibly an .OT slice map —
// all of which must move together, in whichever pool folder they live in. Renaming only
// the audio would orphan the analysis and the loop would then silently refuse to engage
// ("no grid"), which is exactly the class of bug we spent today chasing.
static esp_err_t files_rename_handler(httpd_req_t *req)
{
    char name[32], to[32];
    if (!get_query_param(req, "name", name, sizeof(name)) ||
        !get_query_param(req, "to", to, sizeof(to))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name/to");
        return ESP_FAIL;
    }
    // ids are bare, uppercase and 8.3-friendly — keep the pool's shape enforceable
    int tl = strlen(to);
    if (tl < 1 || tl > 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Name must be 1-8 chars");
        return ESP_FAIL;
    }
    for (int i = 0; i < tl; i++) {
        char c = to[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Use A-Z 0-9 _ only");
            return ESP_FAIL;
        }
    }
    // never clobber: renaming onto an existing id would silently merge two samples
    char probe[300];
    struct stat pst;
    sd_lock_take();
    bool taken = (sample_resolve(to, probe, sizeof(probe)) == 0 && stat(probe, &pst) == 0);
    sd_lock_give();
    if (taken) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Target name already exists");
        return ESP_FAIL;
    }

    static const char *const rn_exts[] = {".RAW", ".WAV", ".AIF", ".AIFF", ".JSN", ".OT"};
    static const char *const rn_dirs[] = {"/sdcard/usr", "/sdcard/usr/REC",
                                          "/sdcard/usr/LOOPS", "/sdcard/usr/SLICES",
                                          "/sdcard/usr/DRUMS", "/sdcard/usr/KEYS",
                                          "/sdcard/usr/TAPE"};
    char from_p[80], to_p[80];
    int moved = 0;
    sd_lock_take();
    for (int d = 0; d < (int)(sizeof(rn_dirs)/sizeof(rn_dirs[0])); d++)
        for (int i = 0; i < 6; i++) {
            snprintf(from_p, sizeof(from_p), "%s/%s%s", rn_dirs[d], name, rn_exts[i]);
            struct stat st;
            if (stat(from_p, &st) != 0) continue;      // this piece does not exist
            snprintf(to_p, sizeof(to_p), "%s/%s%s", rn_dirs[d], to, rn_exts[i]);
            if (rename(from_p, to_p) == 0) moved++;
        }
    sd_lock_give();

    if (!moved) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such sample");
        return ESP_FAIL;
    }

    // the sidecar names ITSELF inside. Leave it stale and the pool disagrees with its
    // own metadata: the browser lists the new id while the JSN still claims the old one.
    char jp[300];
    sample_resolve_aux(to, ".JSN", jp, sizeof(jp));
    cJSON *root = readJSONFileAsCJSON(jp);
    if (root) {
        cJSON_DeleteItemFromObject(root, "id");
        cJSON_AddStringToObject(root, "id", to);
        if (cJSON_GetObjectItemCaseSensitive(root, "name")) {
            cJSON_DeleteItemFromObject(root, "name");
            cJSON_AddStringToObject(root, "name", to);
        }
        char *js = cJSON_Print(root);
        cJSON_Delete(root);
        if (js) {
            writeJSONFile(jp, js);
            free(js);
        }
    }

    ESP_LOGI(TAG, "rename %s -> %s (%d files)", name, to, moved);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ─── GET /files/raw?name=xxx ──────────────────────────────────────────────────

#define STREAM_CHUNK 4096

static esp_err_t files_raw_handler(httpd_req_t *req)
{
    char name[32];
    if (!get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }

    char path[72];
    size_t nl = strlen(name);
    // accept bare ids and full filenames — "REC_0147.RAW" used to become
    // REC_0147.RAW.RAW and 404. Everything goes through the resolvers so
    // foldered files (usr/REC, usr/LOOPS) are found wherever they live.
    char idtmp[32];
    if (nl > 4 && (strcasecmp(name + nl - 4, ".JSN") == 0 ||
                   strcasecmp(name + nl - 3, ".OT") == 0)) {
        int el = (strcasecmp(name + nl - 3, ".OT") == 0) ? 3 : 4;
        snprintf(idtmp, sizeof(idtmp), "%.*s", (int)nl - el, name);
        sample_resolve_aux(idtmp, name + nl - el, path, sizeof(path));
    } else if (sample_name_id(name, idtmp, sizeof(idtmp)))
        sample_resolve(idtmp, path, sizeof(path));   // full audio filename -> id
    else
        sample_resolve(name, path, sizeof(path));    // bare id
    sd_lock_take();
    FILE *f = fopen(path, "rb");
    sd_lock_give();
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    struct stat st;
    sd_lock_take();
    stat(path, &st);
    sd_lock_give();
    char len_str[16];
    snprintf(len_str, sizeof(len_str), "%ld", (long)st.st_size);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Length", len_str);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char *buf = malloc(STREAM_CHUNK);
    if (!buf) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }

    int n;
    for (;;) {
        // read one chunk under the SD lock, then release it before the (slow)
        // network send so audio file-reader refills can interleave
        sd_lock_take();
        n = fread(buf, 1, STREAM_CHUNK, f);
        sd_lock_give();
        if (n <= 0) break;
        // a client that disconnects mid-download must abort the stream, or
        // this loop pumps the rest of the file at a dead socket with the
        // (single-threaded) httpd wedged and the SD bus busy the whole time
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
        vTaskDelay(1);
    }
    httpd_resp_send_chunk(req, NULL, 0);

    sd_lock_take();
    fclose(f);
    sd_lock_give();
    free(buf);
    return ESP_OK;
}

// ─── /screenshot — the live TFT display as a 24-bit BMP ─────────────────────
// The panel's GRAM readback is DEAD on this unit (MISO idles high — the first
// /screenshot attempt came back solid white), so this reads the PSRAM SHADOW
// FRAMEBUFFER instead: tftspi.c write-through hooks keep tft_shadow an exact
// copy of every draw. disp_lock is taken PER ROW purely against tearing (the
// UI task holds it around menuProcessEvent), released between rows so a slow
// client can't freeze the UI. BMP wants B,G,R bottom-up.
static void put_le32(uint8_t *p, uint32_t v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put_le16(uint8_t *p, uint16_t v){ p[0]=v; p[1]=v>>8; }

static esp_err_t screenshot_get_handler(httpd_req_t *req)
{
    int w = _width, h = _height;
    if (w <= 0 || h <= 0 || w > 640 || h > 640) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "bad display dims");
        return ESP_FAIL;
    }
    uint32_t row_bytes = (uint32_t)w * 3;            // 24bpp (w=320 -> 960, already 4-aligned)
    uint32_t pad = (4 - (row_bytes & 3)) & 3;        // BMP rows pad to 4 bytes
    uint32_t img = (row_bytes + pad) * (uint32_t)h;
    uint32_t filesz = 54 + img;

    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    put_le32(hdr + 2,  filesz);
    put_le32(hdr + 10, 54);       // pixel-data offset
    put_le32(hdr + 14, 40);       // DIB header size (BITMAPINFOHEADER)
    put_le32(hdr + 18, (uint32_t)w);
    put_le32(hdr + 22, (uint32_t)h);  // positive height => bottom-up
    put_le16(hdr + 26, 1);        // planes
    put_le16(hdr + 28, 24);       // bpp
    put_le32(hdr + 30, 0);        // BI_RGB (uncompressed)
    put_le32(hdr + 34, img);
    put_le32(hdr + 38, 2835);     // ~72 dpi
    put_le32(hdr + 42, 2835);

    if (!tft_shadow) {
        // LAZY shadow FB: allocating 230 KB PSRAM at boot starved libxmp
        // (tracker "no memory"), so the first /screenshot pays instead. The
        // shadow only mirrors draws made AFTER allocation — kick a redraw of
        // the current page and tell the caller to retry for a full frame.
        tft_shadow_init();
        if (!tft_shadow) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "shadow fb alloc failed");
            return ESP_FAIL;
        }
        ui_ev_ts_t ev = { .event = EV_ENTERED_MENU, .event_data = NULL };
        if (ui_ev_queue) xQueueSend(ui_ev_queue, &ev, 0);
        httpd_resp_set_status(req, "503 Warming");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "shadow fb allocated - redrawing, retry in ~1s\n");
        return ESP_OK;
    }
    uint8_t *out = heap_caps_malloc(row_bytes + pad, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (pad) memset(out + row_bytes, 0, pad);

    char len_str[16];
    snprintf(len_str, sizeof(len_str), "%u", (unsigned)filesz);
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Content-Length", len_str);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    esp_err_t rc = httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));
    for (int y = h - 1; y >= 0 && rc == ESP_OK; y--) {   // bottom-up
        disp_lock_take();
        const color_t *row = tft_shadow + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint8_t *o = out + x * 3;
            o[0] = row[x].b;
            o[1] = row[x].g;
            o[2] = row[x].r;
        }
        disp_lock_give();
        rc = httpd_resp_send_chunk(req, (const char *)out, row_bytes + pad);
        if ((y & 15) == 0) vTaskDelay(1);   // shadow reads are RAM-fast; sparse yields suffice
    }
    httpd_resp_send_chunk(req, NULL, 0);
    free(out);
    return ESP_OK;
}

// ─── /import — convert-on-import (POST = start scan, GET = progress) ─────────

static esp_err_t import_post_handler(httpd_req_t *req)
{
    if (samp_import_start() != 0)
        return send_json(req, "{\"ok\":false,\"why\":\"busy\"}");
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t import_get_handler(httpd_req_t *req)
{
    char j[160];
    snprintf(j, sizeof(j),
             "{\"busy\":%s,\"done\":%d,\"fail\":%d,\"seen\":%d,\"pct\":%d,\"cur\":\"%s\"}",
             samp_import_busy ? "true" : "false",
             samp_import_done, samp_import_fail, samp_import_seen,
             samp_import_pct, samp_import_cur);
    return send_json(req, j);
}

// ─── GET /settings ─────────────────────────────────────────────────────────────

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    cJSON *root = readJSONFileAsCJSON("/sdcard/CONFIG.JSN");
    if (!root) { send_json(req, "{}"); return ESP_OK; }
    cJSON *settings = cJSON_GetObjectItem(root, "settings");
    if (!settings) { cJSON_Delete(root); send_json(req, "{}"); return ESP_OK; }

    cJSON *out = cJSON_CreateObject();
    cJSON *j;
    if ((j = cJSON_GetObjectItem(settings, "ssid")))    cJSON_AddStringToObject(out, "ssid", j->valuestring);
    if ((j = cJSON_GetObjectItem(settings, "apikey")))  cJSON_AddStringToObject(out, "apikey", j->valuestring);
    if ((j = cJSON_GetObjectItem(settings, "hostname"))) cJSON_AddStringToObject(out, "hostname", j->valuestring);
    if ((j = cJSON_GetObjectItem(settings, "tz_shift"))) cJSON_AddNumberToObject(out, "tz_shift", j->valuedouble);
    if ((j = cJSON_GetObjectItem(settings, "txpwr")))   cJSON_AddNumberToObject(out, "txpwr", j->valuedouble);
    // NOTE: password intentionally omitted

    char *s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(root);
    if (s) { send_json(req, s); free(s); } else send_json(req, "{}");
    return ESP_OK;
}

// ─── POST /settings ────────────────────────────────────────────────────────────

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total < 2 || total > 512) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body"); return ESP_FAIL; }
    char *body = malloc(total + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed"); return ESP_FAIL; }
        received += r;
    }
    body[total] = 0;

    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!in) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON"); return ESP_FAIL; }

    cJSON *cfg = readJSONFileAsCJSON("/sdcard/CONFIG.JSN");
    if (!cfg) { cJSON_Delete(in); send_json(req, "{}"); return ESP_OK; }
    cJSON *settings = cJSON_GetObjectItem(cfg, "settings");
    if (!settings) { cJSON_Delete(in); cJSON_Delete(cfg); send_json(req, "{}"); return ESP_OK; }

    bool wifiChanged = false;
    cJSON *j, *cur;
    if ((j = cJSON_GetObjectItem(in, "ssid")) && j->valuestring) {
        cur = cJSON_GetObjectItem(settings, "ssid");
        if (!cur || !cur->valuestring || strcmp(cur->valuestring, j->valuestring) != 0) wifiChanged = true;
        cJSON_ReplaceItemInObject(settings, "ssid", cJSON_CreateString(j->valuestring));
    }
    if ((j = cJSON_GetObjectItem(in, "passwd")) && j->valuestring && strlen(j->valuestring) > 0) {
        cur = cJSON_GetObjectItem(settings, "passwd");
        if (!cur || !cur->valuestring || strcmp(cur->valuestring, j->valuestring) != 0) wifiChanged = true;
        cJSON_ReplaceItemInObject(settings, "passwd", cJSON_CreateString(j->valuestring));
    }
    if ((j = cJSON_GetObjectItem(in, "apikey")) && j->valuestring) {
        cJSON_ReplaceItemInObject(settings, "apikey", cJSON_CreateString(j->valuestring));
        freesoundSetToken(j->valuestring);
    }
    // per-device mDNS/DHCP hostname ("<name>.local"); sanitized to RFC-safe
    if ((j = cJSON_GetObjectItem(in, "hostname")) && j->valuestring) {
        char hn[33];
        int w = 0;
        for (const char *pc = j->valuestring; *pc && w < 32; pc++) {
            char c = *pc;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') hn[w++] = c;
            else if (c >= 'A' && c <= 'Z') hn[w++] = c + 32;
        }
        hn[w] = 0;
        if (hn[0]) {
            cJSON_DeleteItemFromObjectCaseSensitive(settings, "hostname");
            cJSON_AddStringToObject(settings, "hostname", hn);
            wifiApplyHostname(hn);
        }
    }
    // display timezone shift (hours vs UTC), persisted like the System menu's
    // Timezone row; the device clock itself is SNTP/UTC
    if ((j = cJSON_GetObjectItem(in, "tz")) && cJSON_IsNumber(j) &&
        j->valueint >= -12 && j->valueint <= 14) {
        cJSON_DeleteItemFromObjectCaseSensitive(settings, "tz_shift");
        cJSON_AddNumberToObject(settings, "tz_shift", j->valueint);
    }
    // teleremote toggle from the web — same persist+apply the System menu does.
    // (Turning it OFF from the web disables /remote/* but not this endpoint,
    // so the web can always turn it back on.)
    if ((j = cJSON_GetObjectItem(in, "remote")) && cJSON_IsNumber(j)) {
        cJSON_DeleteItemFromObjectCaseSensitive(settings, "remote");
        cJSON_AddNumberToObject(settings, "remote", j->valueint ? 1 : 0);
        rest_remote_enable(j->valueint);
    }
    // optional WiFi TX power cap in quarter-dBm (8..84) — antenna-less units
    // brown out on full-power TX bursts; applied live and persisted
    if ((j = cJSON_GetObjectItem(in, "txpwr")) && cJSON_IsNumber(j) && j->valueint >= 8 && j->valueint <= 84) {
        if (cJSON_GetObjectItem(settings, "txpwr"))
            cJSON_ReplaceItemInObject(settings, "txpwr", cJSON_CreateNumber(j->valueint));
        else
            cJSON_AddNumberToObject(settings, "txpwr", j->valueint);
        wifiApplyTxPower(j->valueint);
    }

    // capture the final credentials before cfg is freed (same flow as the menu
    // settings path: save, then reconnect with the new config)
    wifi_config_t wifi_config;
    if (wifiChanged) {
        memset(&wifi_config, 0, sizeof(wifi_config));
        if ((cur = cJSON_GetObjectItem(settings, "ssid")) && cur->valuestring)
            strlcpy((char*)wifi_config.sta.ssid, cur->valuestring, sizeof(wifi_config.sta.ssid));
        if ((cur = cJSON_GetObjectItem(settings, "passwd")) && cur->valuestring)
            strlcpy((char*)wifi_config.sta.password, cur->valuestring, sizeof(wifi_config.sta.password));
    }

    char *s = cJSON_Print(cfg);
    cJSON_Delete(in);
    cJSON_Delete(cfg);
    if (s) { writeJSONFile("/sdcard/CONFIG.JSN", s); free(s); }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    if (wifiChanged) {
        ESP_LOGI(TAG, "WiFi settings changed via web — reconnecting");
        vTaskDelay(pdMS_TO_TICKS(500));   // let the response reach the client before WiFi drops
        restartWifi(&wifi_config);
    }
    return ESP_OK;
}

// ─── GET /status ───────────────────────────────────────────────────────────────

// declared locally rather than pulling components/fxrack into this component's
// REQUIRES for one integer — see fxrack.h for what it measures
int fxrack_peak_pct(bool clear);

static esp_err_t status_get_handler(httpd_req_t *req)
{
    audio_status_t st;
    audio_get_status(&st);
    bool rec = recording_is_active();
    const machine_t *m = machine_active();

    bl_status_t bl;
    beatlisten_get_status(&bl);

    // tuner: one extra object, and only while the service is actually on —
    // this is the 500 ms hot poll, so an idle tuner costs the poll nothing
    tuner_status_t tu;
    tuner_get_status(&tu);
    char tun[80] = "";
    if (tu.on)
        snprintf(tun, sizeof(tun), ",\"tun\":{\"hv\":%d,\"hz\":%.2f,\"n\":%d,\"c\":%.1f,\"cf\":%.2f,\"us\":%d}",
                 tu.have ? 1 : 0, (double)tu.hz, tu.midi, (double)tu.cents,
                 (double)tu.conf, tu.cost_us);

    // build compact JSON by hand to avoid cJSON overhead in hot path
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"machine\":\"%s\",\"recording\":%s,\"v0\":\"%s\",\"v1\":\"%s\","
        "\"cv\":[%u,%u,%u,%u,%u,%u,%u,%u],\"trig\":%u,"
        "\"vu\":[%u,%u,%u,%u],"
        "\"bl\":{\"m\":%d,\"st\":%d,\"bpm\":%.2f,\"cf\":%.2f,\"us\":%d},\"aus\":%u,\"auspk\":%u,\"fxpk\":%d%s}",
        m ? m->name : "",
        rec ? "true" : "false",
        st.v0, st.v1,
        st.cv[0], st.cv[1], st.cv[2], st.cv[3],
        st.cv[4], st.cv[5], st.cv[6], st.cv[7],
        st.trig,
        st.vu[0], st.vu[1], st.vu[2], st.vu[3],
        bl.mode, bl.state, (double)bl.bpm, (double)bl.conf, bl.cost_us,
        audio_proc_us(), audio_proc_peak_us(true), fxrack_peak_pct(true), tun);
    (void)n;
    send_json(req, buf);
    return ESP_OK;
}

// ─── PUT /drop_sample (existing upload, kept unchanged) ─────────────────────

static esp_err_t drop_sample_put_handler(httpd_req_t *req)
{
    int ret, remaining = req->content_len, total = 0;
    char *buf;
    size_t buf_len;
    FIL raw_file;
    UINT bw;
    ui_ev_ts_t ev;
    int file_len_d100 = req->content_len / 100;
    char file_name[32] = "";
    char file_name_jsn[32] = "";
    cJSON *val;
    cJSON *root = cJSON_CreateObject();

    buf_len = httpd_req_get_hdr_value_len(req, "Name") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Name", buf, buf_len) == ESP_OK) {
            cleanStringSpace(buf);
            sprintf(file_name, "%s.raw", buf);
            val = cJSON_CreateString(file_name);
            cJSON_AddItemToObject(root, "name", val);
            sprintf(file_name, "%s", buf);
            val = cJSON_CreateString(file_name);
            cJSON_AddItemToObject(root, "id", val);
            sprintf(file_name, "/usr/%s.RAW", buf);
            sprintf(file_name_jsn, "/sdcard/usr/%s.JSN", buf);
        }
        free(buf);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, 0);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Description") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Description", buf, buf_len) == ESP_OK) { cleanString(buf); val = cJSON_CreateString(buf); cJSON_AddItemToObject(root, "description", val); }
        free(buf);
    } else { cJSON_AddStringToObject(root, "description", ""); }

    buf_len = httpd_req_get_hdr_value_len(req, "Tags") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Tags", buf, buf_len) == ESP_OK) { cleanString(buf); val = cJSON_CreateString(buf); cJSON_AddItemToObject(root, "tags_s", val); }
        free(buf);
    } else { cJSON_AddStringToObject(root, "tags_s", ""); }

    // FS_LOCK makes this fail with FR_LOCKED if a voice is streaming the file —
    // refuse rather than truncate a sample that is currently playing
    sd_lock_take();
    FRESULT fr = f_open(&raw_file, file_name, FA_CREATE_ALWAYS | FA_WRITE);
    sd_lock_give();
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "drop_sample: f_open %s failed (%d)", file_name, fr);
        cJSON_Delete(root);
        httpd_resp_send_err(req, fr == FR_LOCKED ? HTTPD_400_BAD_REQUEST : HTTPD_500_INTERNAL_SERVER_ERROR,
                            fr == FR_LOCKED ? "File in use" : "SD open failed");
        return ESP_FAIL;
    }

    buf = malloc(4096);
    remaining = req->content_len;

    int timeouts = 0;
    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, 4096))) <= 0) {
            // bounded timeout retries: a client that silently vanished used to
            // spin this loop forever, wedging the worker with the file open
            if (ret == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) continue;
            sd_lock_take();
            f_close(&raw_file); f_unlink(file_name);
            sd_lock_give();
            free(buf); cJSON_Delete(root);
            return ESP_FAIL;
        }
        timeouts = 0;
        total += ret; remaining -= ret;
        if (ret > 0) {
            sd_lock_take();
            f_write(&raw_file, buf, ret, &bw);
            sd_lock_give();
        }
        ev.event = EV_DECODING_PROGRESS;
        ev.event_data = (void *)(total / (file_len_d100 ? file_len_d100 : 1));
        xQueueSend(ui_ev_queue, &ev, portMAX_DELAY);
    }

    // frame-align by padding at the END — the old leading pad shifted every
    // non-4-multiple upload by 1-3 bytes, which was invisible for RAW audio
    // but corrupted byte-exact containers (bench: every MP3 upload arrived
    // with 0x00 bytes prepended and helix refused the stream)
    if (req->content_len % 4 != 0) {
        int pad = 4 - (req->content_len % 4);
        const char zeros[3] = {0};
        sd_lock_take();
        f_write(&raw_file, zeros, pad, &bw);
        sd_lock_give();
    }

    cJSON_AddStringToObject(root, "username", "myself");
    cJSON_AddStringToObject(root, "url", "local");
    cJSON_AddStringToObject(root, "license", "own license");
    writeJSONFile(file_name_jsn, cJSON_Print(root));

    sd_lock_take();
    f_close(&raw_file);
    sd_lock_give();
    free(buf);
    cJSON_Delete(root);

    // convert-on-import: the upload landed verbatim as <name>.RAW — kick the
    // background importer to sniff and convert it. NOT done inline: helix +
    // the resampler need a ~20 KB stack and seconds of runtime; the httpd
    // worker has neither (the import task owns both). Native uploads pass
    // the scan untouched.
    samp_import_start();

    httpd_resp_send(req, NULL, 0);
    ev.event = EV_DECODING_DONE;
    xQueueSend(ui_ev_queue, &ev, portMAX_DELAY);
    return ESP_OK;
}

// ─── GET /sysinfo ─────────────────────────────────────────────────────────────
// Device IP + SD free/total bytes. Fetched on-demand by the web page (not the
// hot /status poll), so the one-off f_getfree FAT scan is fine.

static esp_err_t sysinfo_get_handler(httpd_req_t *req)
{
    char ip[20] = {0};
    wifiGetIPString(ip, sizeof(ip));

    uint64_t freeb = 0, totb = 0;
    FATFS *fs = NULL;
    DWORD fre_clust = 0;
    sd_lock_take();
    if (f_getfree("0:", &fre_clust, &fs) == FR_OK && fs) {
        uint64_t bytes_per_clust = (uint64_t)fs->csize * 512; // SD sectors are 512B
        totb  = (uint64_t)(fs->n_fatent - 2) * bytes_per_clust;
        freeb = (uint64_t)fre_clust * bytes_per_clust;
    }
    sd_lock_give();

    // registry names for the Remote tab (Stub stays hidden here too)
    char machines[192] = "";
    int mp = 0;
    for (int i = 0; machine_registry[i] != NULL; i++) {
        if (strcmp(machine_registry[i]->name, "Stub") == 0) continue;
        mp += snprintf(machines + mp, sizeof(machines) - mp, "%s\"%s\"",
                       mp ? "," : "", machine_registry[i]->name);
        if (mp >= (int)sizeof(machines) - 1) break;
    }

    // device clock (SNTP, UTC) + the display timezone shift from settings —
    // servers-facing features (radio, certs) will lean on this being right
    int tzs = 0;
    {
        cJSON *root = readJSONFileAsCJSON("/sdcard/CONFIG.JSN");
        cJSON *st = root ? cJSON_GetObjectItemCaseSensitive(root, "settings") : NULL;
        cJSON *tj = st ? cJSON_GetObjectItemCaseSensitive(st, "tz_shift") : NULL;
        if (tj && cJSON_IsNumber(tj)) tzs = tj->valueint;
        if (root) cJSON_Delete(root);
    }

    // RAM report (on-demand only — never the hot /status poll). "big" is the
    // largest single block, which is what actually decides whether a slab-sized
    // alloc (tape banks, reverb/delay slabs) succeeds or silently fail-softs.
    unsigned pfree = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    unsigned pbig  = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    unsigned ptot  = (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    unsigned ifree = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    unsigned ibig  = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    char buf[720];
    snprintf(buf, sizeof(buf),
             "{\"ip\":\"%s\",\"free\":%llu,\"total\":%llu,\"remote\":%d,"
             "\"version\":\"%s\",\"blisten\":%d,\"blout\":%d,"
             "\"psram\":{\"free\":%u,\"big\":%u,\"total\":%u},"
             "\"iram\":{\"free\":%u,\"big\":%u},"
             "\"time\":%ld,\"tz\":%d,\"machines\":[%s]}",
             ip, (unsigned long long)freeb, (unsigned long long)totb,
             s_remote_on ? 1 : 0,
             STRAMPLER_FW_VERSION, beatlisten_get_mode(), beatlisten_get_out(),
             pfree, pbig, ptot, ifree, ibig,
             (long)time(NULL), tzs, machines);
    send_json(req, buf);
    return ESP_OK;
}

// ─── PUT /bootlogo ────────────────────────────────────────────────────────────
// Accepts a bootlogo.bmp and writes it to the SD root (read at every boot by
// ui.c). The browser converts any image to the exact legacy format first;
// here we just enforce it: 320x240, 24-bit, classic 54-byte header, exact
// size — the boot-time BMP loader silently shows nothing for anything else.

#define BOOTLOGO_SIZE 230454

static esp_err_t bootlogo_put_handler(httpd_req_t *req)
{
    if (req->content_len != BOOTLOGO_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a 320x240x24 bootlogo BMP");
        return ESP_FAIL;
    }
    char *buf = malloc(4096);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }

    sd_lock_take();
    FILE *f = fopen("/sdcard/bootlogo.bmp", "wb");
    sd_lock_give();
    if (!f) { free(buf); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD open failed"); return ESP_FAIL; }

    int remaining = req->content_len, total = 0;
    bool ok = true;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining > 4096 ? 4096 : remaining);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { ok = false; break; }
        if (total == 0) {
            // header sanity: 'BM', pixel offset 54, 320x240, 24 bpp, uncompressed
            if (r < 34 || buf[0] != 'B' || buf[1] != 'M' ||
                *(uint32_t *)(buf + 10) != 54 ||
                *(int32_t *)(buf + 18) != 320 || *(int32_t *)(buf + 22) != 240 ||
                *(uint16_t *)(buf + 28) != 24 || *(uint32_t *)(buf + 30) != 0) {
                ok = false;
                break;
            }
        }
        sd_lock_take();
        size_t w = fwrite(buf, 1, r, f);
        sd_lock_give();
        if ((int)w != r) { ok = false; break; }
        total += r;
        remaining -= r;
    }
    sd_lock_take();
    fclose(f);
    if (!ok) remove("/sdcard/bootlogo.bmp");   // don't leave a broken logo behind
    sd_lock_give();
    free(buf);

    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid bootlogo upload");
        return ESP_FAIL;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ─── teleremote: /remote/* ────────────────────────────────────────────────────
// Always-on core endpoints (gated by the System→Settings→Remote toggle):
// encoder events into the UI queue, soft trigger pulses into the audio task,
// and machine switching via the UI task (same path as a front-panel switch).

static esp_err_t remote_gate(httpd_req_t *req)
{
    if (s_remote_on) return ESP_OK;
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"error\":\"remote disabled on device\"}");
    return ESP_FAIL;
}

static esp_err_t remote_event_handler(httpd_req_t *req)
{
    if (remote_gate(req) != ESP_OK) return ESP_OK;
    char evs[12];
    if (!get_query_param(req, "ev", evs, sizeof(evs))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ev");
        return ESP_FAIL;
    }
    int ev;
    if      (strcmp(evs, "fwd")   == 0) ev = EV_FWD;
    else if (strcmp(evs, "bwd")   == 0) ev = EV_BWD;
    else if (strcmp(evs, "press") == 0) ev = EV_SHORT_PRESS;
    else if (strcmp(evs, "long")  == 0) ev = EV_LONG_PRESS;
    else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad ev");
        return ESP_FAIL;
    }
    ui_ev_ts_t uev = { .event = ev, .event_data = NULL };
    xQueueSend(ui_ev_queue, &uev, 0);
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t remote_trig_handler(httpd_req_t *req)
{
    if (remote_gate(req) != ESP_OK) return ESP_OK;
    char ts[8], ms_s[8];
    if (!get_query_param(req, "t", ts, sizeof(ts)) || (ts[0] != '1' && ts[0] != '2') || ts[1]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "t must be 1 or 2");
        return ESP_FAIL;
    }
    int ms = 40;
    if (get_query_param(req, "ms", ms_s, sizeof(ms_s))) ms = atoi(ms_s);
    if (ms < 5) ms = 5;
    if (ms > 2000) ms = 2000;
    audio_remote_trig(ts[0] - '1', ms);
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /remote/cv?ch=1..8&v=0..4095[&ms=..] — the knob mirror of /remote/trig:
// while fresh, the value substitutes for the ADC in the audio task, then decays
// back to the physical knob. This is what makes the web page an instrument.
static esp_err_t remote_cv_handler(httpd_req_t *req)
{
    if (remote_gate(req) != ESP_OK) return ESP_OK;
    char chs[8], vs[8], ms_s[8];
    if (!get_query_param(req, "ch", chs, sizeof(chs))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ch must be 1..8");
        return ESP_FAIL;
    }
    int ch = atoi(chs);
    if (ch < 1 || ch > 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ch must be 1..8");
        return ESP_FAIL;
    }
    if (!get_query_param(req, "v", vs, sizeof(vs))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "v must be 0..4095");
        return ESP_FAIL;
    }
    int v = atoi(vs);
    if (v < 0 || v > 4095) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "v must be 0..4095");
        return ESP_FAIL;
    }
    int ms = 250;                       // a dragging web knob re-posts well within this
    if (get_query_param(req, "ms", ms_s, sizeof(ms_s))) ms = atoi(ms_s);
    if (ms < 20) ms = 20;
    if (ms > 5000) ms = 5000;
    audio_remote_cv(ch - 1, v, ms);
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /blisten?mode=0..4 | ?out=0..2 | ?relock=1 — remote control of the
// beat listener (mode/clock-out persist into CONFIG.JSN settings like the
// System menu does; relock is momentary). Any combination of params works.
static esp_err_t blisten_post_handler(httpd_req_t *req)
{
    char v[8];
    bool did = false, persist = false;
    if (get_query_param(req, "mode", v, sizeof(v))) {
        int m = atoi(v);
        if (m < 0 || m >= BL_NMODES) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode 0..4");
            return ESP_FAIL;
        }
        beatlisten_set_mode(m);
        did = persist = true;
    }
    if (get_query_param(req, "out", v, sizeof(v))) {
        int o = atoi(v);
        if (o < 0 || o > 2) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "out 0..2");
            return ESP_FAIL;
        }
        beatlisten_set_out(o);
        did = persist = true;
    }
    if (get_query_param(req, "relock", v, sizeof(v)) && v[0] == '1') {
        beatlisten_relock();
        did = true;
    }
    if (!did) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need mode/out/relock");
        return ESP_FAIL;
    }
    if (persist) {
        cJSON *root = readJSONFileAsCJSON("/sdcard/CONFIG.JSN");
        cJSON *settings = root ? cJSON_GetObjectItemCaseSensitive(root, "settings") : NULL;
        if (settings) {
            cJSON_DeleteItemFromObjectCaseSensitive(settings, "blisten");
            cJSON_AddNumberToObject(settings, "blisten", beatlisten_get_mode());
            cJSON_DeleteItemFromObjectCaseSensitive(settings, "blisten_out");
            cJSON_AddNumberToObject(settings, "blisten_out", beatlisten_get_out());
            char *s = cJSON_Print(root);
            if (s) { writeJSONFile("/sdcard/CONFIG.JSN", s); free(s); }
        }
        if (root) cJSON_Delete(root);
    }
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// GET = the active machine's full settings (same JSON as its autosave state);
// POST = apply edited settings via preset_load on the UI task + autosave
static esp_err_t remote_params_get_handler(httpd_req_t *req)
{
    if (remote_gate(req) != ESP_OK) return ESP_OK;
    const machine_t *m = machine_active();
    if (!m || !m->preset_save) { send_json(req, "{}"); return ESP_OK; }
    cJSON *o = m->preset_save();
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    if (s) { send_json(req, s); free(s); } else send_json(req, "{}");
    return ESP_OK;
}

static esp_err_t remote_params_post_handler(httpd_req_t *req)
{
    if (remote_gate(req) != ESP_OK) return ESP_OK;
    int total = req->content_len;
    if (total < 2 || total > 8192) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body"); return ESP_FAIL; }
    char *body = malloc(total + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed"); return ESP_FAIL; }
        received += r;
    }
    body[total] = 0;

    cJSON *chk = cJSON_Parse(body);      // reject garbage before queueing
    if (!chk) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON"); return ESP_FAIL; }
    cJSON_Delete(chk);

    ui_ev_ts_t uev = { .event = EV_REMOTE_PRESET, .event_data = body };
    if (xQueueSend(ui_ev_queue, &uev, 0) != pdTRUE) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Queue full");
        return ESP_FAIL;
    }
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t remote_machine_handler(httpd_req_t *req)
{
    if (remote_gate(req) != ESP_OK) return ESP_OK;
    char name[16];
    if (!get_query_param(req, "name", name, sizeof(name)) || !name[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }
    if (!machine_by_name(name) || strcmp(name, "Stub") == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such machine");
        return ESP_FAIL;
    }
    // switch on the UI task so autosave/activate/rebind matches the front panel
    ui_ev_ts_t uev = { .event = EV_REMOTE_MACHINE, .event_data = strdup(name) };
    if (!uev.event_data || xQueueSend(ui_ev_queue, &uev, 0) != pdTRUE) {
        free(uev.event_data);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Queue full");
        return ESP_FAIL;
    }
    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

// ─── machine-contributed endpoints ───────────────────────────────────────────
// The active machine may publish extra URIs (machine_ui_t.web_uris); they are
// registered on activate and dropped on deactivate. We remember what we
// registered so a machine switch swaps them cleanly, and the last-seen machine
// so URIs registered before WiFi/httpd came up are applied when it does.

#define MAX_MACHINE_URIS 8
static const httpd_uri_t *machine_uris[MAX_MACHINE_URIS];
static int n_machine_uris = 0;
static const machine_t *web_machine = NULL;

static void machine_web_apply(const machine_t *m)
{
    web_machine = m;
    if (!server) return;
    for (int i = 0; i < n_machine_uris; i++)
        httpd_unregister_uri_handler(server, machine_uris[i]->uri, machine_uris[i]->method);
    n_machine_uris = 0;
    if (!m || !m->ui || !m->ui->web_uris) return;
    const httpd_uri_t *u = (const httpd_uri_t *)m->ui->web_uris;
    for (int i = 0; i < m->ui->n_web_uris && n_machine_uris < MAX_MACHINE_URIS; i++)
        if (httpd_register_uri_handler(server, &u[i]) == ESP_OK)
            machine_uris[n_machine_uris++] = &u[i];
    if (n_machine_uris)
        ESP_LOGI(TAG, "%s: %d machine URIs registered", m->name, n_machine_uris);
}

// ─── tracker module files (usr/TRACKER) ─────────────────────────────────────────
// Upload / list / download / delete for tracker modules. These live in the
// CORE (not the Tracker machine) so they work regardless of the active machine
// — a module is just a file. Path mirrors tracker_priv.h's TRK_DIR_* (the
// on-device Module browser reads the same folder).
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#define MOD_DIR_VFS  "/sdcard/usr/TRACKER"    // was usr/MODS (see tracker_priv.h)
#define MOD_DIR_FAT  "/usr/TRACKER"
#define MOD_TMP      "/usr/TRACKER/UPLOAD.TMP"   // atomic-upload scratch (8.3-safe)
#define MOD_MAX_FILE (2 * 1024 * 1024)

static const char *const MOD_UP_EXTS[] = {
    "MOD","XM","IT","S3M","669","MTM","OKT","ULT","FAR","MED","DBM","AMF",
    "PTM","STM","DMF","GDM","IMF","LIQ","MDL","PT3","OXM","DIGI","EMOD",
};
static bool mod_ext_ok(const char *e){
    for (int i = 0; i < (int)(sizeof(MOD_UP_EXTS)/sizeof(MOD_UP_EXTS[0])); i++)
        if (strcasecmp(e, MOD_UP_EXTS[i]) == 0) return true;
    return false;
}
// reject path traversal / subdirs in a query-supplied filename
static bool mod_name_safe(const char *n){
    if (!n[0]) return false;
    for (const char *p = n; *p; p++) if (*p == '/' || *p == '\\') return false;
    return strstr(n, "..") == NULL;
}

// PUT /trk/upload — headers Name (8.3 base, clamped) + Ext; body = raw bytes,
// stored verbatim (no conversion) to usr/TRACKER/<NAME>.<EXT>.
static esp_err_t mod_upload_handler(httpd_req_t *req)
{
    char name[16] = "", ext[8] = "";
    size_t nl = httpd_req_get_hdr_value_len(req, "Name") + 1;
    if (nl > 1) {
        char *b = malloc(nl);
        if (httpd_req_get_hdr_value_str(req, "Name", b, nl) == ESP_OK) {
            cleanStringSpace(b);
            if (nl > 9) b[8] = 0;                    // 8.3 clamp (b may be shorter)
            strlcpy(name, b, sizeof(name));
        }
        free(b);
    }
    size_t el = httpd_req_get_hdr_value_len(req, "Ext") + 1;
    if (el > 1) {
        char *b = malloc(el);
        if (httpd_req_get_hdr_value_str(req, "Ext", b, el) == ESP_OK) strlcpy(ext, b, sizeof(ext));
        free(b);
    }
    if (!name[0] || !mod_ext_ok(ext)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad name/ext"); return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > MOD_MAX_FILE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Module too big or empty"); return ESP_FAIL;
    }

    char up[8]; strlcpy(up, ext, sizeof(up));
    for (char *p = up; *p; p++) *p = toupper((unsigned char)*p);
    char path[48];
    snprintf(path, sizeof(path), MOD_DIR_FAT "/%s.%s", name, up);

    // ATOMIC WRITE: stream to a temp file, rename to the real name only after
    // the full body arrives. A partial/interrupted upload leaves only MOD_TMP
    // (which no machine loads), so the tracker never sees a truncated module —
    // truncated modules can crash libxmp. (8.3-safe fixed temp; uploads are
    // serialized by the single-threaded httpd so there's no collision.)
    FIL f;
    sd_lock_take();
    f_mkdir(MOD_DIR_FAT);                    // ensure the folder exists (ok if present)
    FRESULT fr = f_open(&f, MOD_TMP, FA_CREATE_ALWAYS | FA_WRITE);
    sd_lock_give();
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "module f_open %s failed (%d)", MOD_TMP, fr);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD open failed"); return ESP_FAIL;
    }

    char *buf = malloc(4096);
    int remaining = req->content_len, ret; UINT bw;
    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, 4096))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            sd_lock_take(); f_close(&f); f_unlink(MOD_TMP); sd_lock_give();
            free(buf); return ESP_FAIL;      // drop the partial temp
        }
        remaining -= ret;
        sd_lock_take(); f_write(&f, buf, ret, &bw); sd_lock_give();
    }
    sd_lock_take();
    f_close(&f);
    f_unlink(path);                          // replace any existing same-named module
    FRESULT rr = f_rename(MOD_TMP, path);     // publish atomically
    if (rr != FR_OK) f_unlink(MOD_TMP);
    sd_lock_give();
    free(buf);
    if (rr != FR_OK) {
        ESP_LOGE(TAG, "module rename %s -> %s failed (%d)", MOD_TMP, path, rr);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD rename failed"); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "module upload %s (%d bytes)", path, (int)req->content_len);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// PUT /drop_ot — header Name = sample basename; body = an Octatrack .ot
// slice sidecar (832 bytes), stored VERBATIM as usr/<NAME>.OT. The slicer
// auto-applies it the next time that sample loads. Bounded timeout retries
// and atomic temp+rename like the module upload.
#define OT_TMP "/usr/OT_UP.TMP"
static esp_err_t drop_ot_put_handler(httpd_req_t *req)
{
    char name[24] = "";   // matches sample-id length (usr/<name>.RAW pairing)
    size_t nl = httpd_req_get_hdr_value_len(req, "Name") + 1;
    if (nl > 1) {
        char *b = malloc(nl);
        if (httpd_req_get_hdr_value_str(req, "Name", b, nl) == ESP_OK) {
            cleanStringSpace(b); strlcpy(name, b, sizeof(name));   // strlcpy truncates
        }
        free(b);
    }
    if (!name[0] || !mod_name_safe(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad name"); return ESP_FAIL;
    }
    if (req->content_len != 832) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ".ot must be 832 bytes"); return ESP_FAIL;
    }

    char path[48];
    snprintf(path, sizeof(path), "/usr/%s.OT", name);
    FIL f;
    sd_lock_take();
    FRESULT fr = f_open(&f, OT_TMP, FA_CREATE_ALWAYS | FA_WRITE);
    sd_lock_give();
    if (fr != FR_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD open failed"); return ESP_FAIL;
    }
    char buf[832];
    int remaining = req->content_len, ret, timeouts = 0; UINT bw;
    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) continue;
            sd_lock_take(); f_close(&f); f_unlink(OT_TMP); sd_lock_give();
            return ESP_FAIL;
        }
        timeouts = 0;
        remaining -= ret;
        sd_lock_take(); f_write(&f, buf, ret, &bw); sd_lock_give();
    }
    sd_lock_take();
    f_close(&f);
    f_unlink(path);
    FRESULT rr = f_rename(OT_TMP, path);
    if (rr != FR_OK) f_unlink(OT_TMP);
    sd_lock_give();
    if (rr != FR_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD rename failed"); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ot upload %s", path);
    // a sample with a slice map belongs in usr/SLICES (Arlo) — sweep every
    // piece there now that the .OT exists (no-op if it already lives there)
    files_move_pieces(name, 3 /* SLICES, mv_dirs[] */);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// GET /trk/list — JSON {"modules":[{"name","size"}...]} from usr/TRACKER
static esp_err_t mod_list_handler(httpd_req_t *req)
{
    char *out = malloc(4096);
    if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    int len = snprintf(out, 4096, "{\"modules\":[");
    sd_lock_take();
    DIR *d = opendir(MOD_DIR_VFS);
    if (d) {
        struct dirent *e; struct stat st; char p[300]; bool first = true;
        while ((e = readdir(d)) != NULL && len < 4096 - 160) {
            if (e->d_name[0] == '.') continue;
            if (strcasecmp(e->d_name, "UPLOAD.TMP") == 0) continue;   // hide upload scratch
            snprintf(p, sizeof(p), MOD_DIR_VFS "/%s", e->d_name);
            long sz = (stat(p, &st) == 0) ? (long)st.st_size : 0;
            len += snprintf(out + len, 4096 - len, "%s{\"name\":\"%s\",\"size\":%ld}",
                            first ? "" : ",", e->d_name, sz);
            first = false;
        }
        closedir(d);
    }
    sd_lock_give();
    len += snprintf(out + len, 4096 - len, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, out, len);
    free(out);
    return ESP_OK;
}

// GET /trk/get?name=FOO.MOD — stream a module file back verbatim
static esp_err_t mod_get_handler(httpd_req_t *req)
{
    char name[32];
    if (!get_query_param(req, "name", name, sizeof(name)) || !mod_name_safe(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad name"); return ESP_FAIL;
    }
    char path[80];
    snprintf(path, sizeof(path), MOD_DIR_VFS "/%s", name);
    sd_lock_take(); FILE *f = fopen(path, "rb"); sd_lock_give();
    if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found"); return ESP_FAIL; }

    struct stat st; sd_lock_take(); stat(path, &st); sd_lock_give();
    char len_str[16]; snprintf(len_str, sizeof(len_str), "%ld", (long)st.st_size);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Length", len_str);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char *buf = malloc(STREAM_CHUNK);
    if (!buf) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    int n;
    for (;;) {
        sd_lock_take(); n = fread(buf, 1, STREAM_CHUNK, f); sd_lock_give();
        if (n <= 0) break;
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
        vTaskDelay(1);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    sd_lock_take(); fclose(f); sd_lock_give();
    free(buf);
    return ESP_OK;
}

// DELETE /trk/delete?name=FOO.MOD
static esp_err_t mod_delete_handler(httpd_req_t *req)
{
    char name[32];
    if (!get_query_param(req, "name", name, sizeof(name)) || !mod_name_safe(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad name"); return ESP_FAIL;
    }
    char path[80];
    snprintf(path, sizeof(path), MOD_DIR_VFS "/%s", name);
    sd_lock_take(); int r = remove(path); sd_lock_give();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (r == 0) { httpd_resp_sendstr(req, "{}"); return ESP_OK; }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

// ─── bounce: record the machine output bus to a pool take ────────────────────
static esp_err_t bounce_start_handler(httpd_req_t *req)
{
    audio_bounce_start();
    return send_json(req, audio_bounce_active() ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"busy\"}");
}
static esp_err_t bounce_stop_handler(httpd_req_t *req)
{
    audio_bounce_stop();
    return send_json(req, "{\"ok\":true}");
}
static esp_err_t bounce_state_handler(httpd_req_t *req)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"active\":%s,\"drops\":%u}",
             audio_bounce_active() ? "true" : "false", (unsigned)recording_get_drops());
    return send_json(req, buf);
}

static esp_err_t bcast_state_handler(httpd_req_t *req)
{
    char buf[140];
    snprintf(buf, sizeof(buf), "{\"enabled\":%s,\"live\":%s,\"diag\":\"%s\",\"enc_us\":%u}",
             audio_broadcast_enabled() ? "true" : "false",
             audio_broadcast_active() ? "true" : "false", audio_broadcast_diag(),
             (unsigned)audio_broadcast_enc_us());
    return send_json(req, buf);
}

// GET /bcast/enable?on=0|1 — the :8000 listener is OFF at boot (its 12 KB task
// stack is INTERNAL RAM the tracker's render task needs). Turn it on here right
// before opening the stream; persisted as settings.broadcast so it survives reboot.
static esp_err_t bcast_enable_handler(httpd_req_t *req)
{
    char v[8];
    if (!get_query_param(req, "on", v, sizeof(v)))
        return send_json(req, "{\"err\":\"need ?on=0|1\"}");
    int on = atoi(v) ? 1 : 0;
    audio_broadcast_set_enabled(on);
    configSetIntSetting("broadcast", on);
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"enabled\":%s}", on ? "true" : "false");
    return send_json(req, buf);
}

// GET /tuner/enable?on=0|1 — the line-in tuner service. Normally the System >
// Tuner page owns this (on when open, off when shut); this endpoint is for the
// web UI and for testing it without standing at the module. NOT persisted: a
// tuner is a thing you open, not a mode you leave running.
static esp_err_t tuner_enable_handler(httpd_req_t *req)
{
    char v[8];
    if (!get_query_param(req, "on", v, sizeof(v)))
        return send_json(req, "{\"err\":\"need ?on=0|1\"}");
    int on = atoi(v) ? 1 : 0;
    tuner_set_enabled(on);
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"enabled\":%s}", tuner_get_enabled() ? "true" : "false");
    return send_json(req, buf);
}

// ─── /ws/midi + /midi/* — the web MIDI bridge (musical typing / WebMIDI) ────
// WS frames are tiny text: "n <note> [vel]" on, "f <note>" off, "x" all off,
// "h" heartbeat (liveness while notes are held). POST fallback mirrors them.
// Gated by the same Remote switch as the teleremote.

static void midi_msg(const char *m)
{
    if (m[0] == 'n')      audio_midi_note_on(atoi(m + 1), 100);
    else if (m[0] == 'f') audio_midi_note_off(atoi(m + 1));
    else if (m[0] == 'x') audio_midi_all_off();
    else                  audio_midi_touch();          // heartbeat
}

static esp_err_t midi_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) return ESP_OK;        // WS handshake done
    if (!s_remote_on) return ESP_OK;                   // remote disabled: ignore
    uint8_t buf[24] = {0};
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = buf };
    if (httpd_ws_recv_frame(req, &f, sizeof(buf) - 1) != ESP_OK) return ESP_FAIL;
    midi_msg((const char *)buf);
    return ESP_OK;
}

static esp_err_t midi_post_handler(httpd_req_t *req)
{
    if (!s_remote_on) { httpd_resp_sendstr(req, "{\"ok\":false}"); return ESP_OK; }
    char note[8] = "";
    get_query_param(req, "note", note, sizeof(note));
    if (strstr(req->uri, "/midi/on"))          { char m[12]; snprintf(m, sizeof(m), "n%s", note); midi_msg(m); }
    else if (strstr(req->uri, "/midi/off"))    { char m[12]; snprintf(m, sizeof(m), "f%s", note); midi_msg(m); }
    else if (strstr(req->uri, "/midi/alloff")) midi_msg("x");
    return send_json(req, "{\"ok\":true}");
}

// ─── /ice/* — icecast push (module as a SOURCE client) ──────────────────────
// Config lives in usr/ICECAST.JSN so the web form prefills; /ice/start with
// any param overrides + re-saves. Always-on core endpoints (like /bounce).
#define ICE_CFG_PATH "/sdcard/usr/ICECAST.JSN"

// httpd_query_key_value does NOT url-decode — %20 etc. arrive literal
static void urldecode_inplace(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else *o++ = *s++;
    }
    *o = 0;
}

static esp_err_t ice_start_handler(httpd_req_t *req)
{
    char host[64] = "", ports[8] = "", mount[48] = "", pass[48] = "", name[48] = "";
    cJSON *saved = readJSONFileAsCJSON(ICE_CFG_PATH);
    if (saved) {
        cJSON *j;
        if ((j = cJSON_GetObjectItemCaseSensitive(saved, "host"))  && cJSON_IsString(j)) strlcpy(host,  j->valuestring, sizeof(host));
        if ((j = cJSON_GetObjectItemCaseSensitive(saved, "port"))  && cJSON_IsNumber(j)) snprintf(ports, sizeof(ports), "%d", j->valueint);
        if ((j = cJSON_GetObjectItemCaseSensitive(saved, "mount")) && cJSON_IsString(j)) strlcpy(mount, j->valuestring, sizeof(mount));
        if ((j = cJSON_GetObjectItemCaseSensitive(saved, "pass"))  && cJSON_IsString(j)) strlcpy(pass,  j->valuestring, sizeof(pass));
        if ((j = cJSON_GetObjectItemCaseSensitive(saved, "name"))  && cJSON_IsString(j)) strlcpy(name,  j->valuestring, sizeof(name));
        cJSON_Delete(saved);
    }
    if (get_query_param(req, "host",  host,  sizeof(host)))  urldecode_inplace(host);
    if (get_query_param(req, "port",  ports, sizeof(ports))) urldecode_inplace(ports);
    if (get_query_param(req, "mount", mount, sizeof(mount))) urldecode_inplace(mount);
    if (get_query_param(req, "pass",  pass,  sizeof(pass)))  urldecode_inplace(pass);
    if (get_query_param(req, "name",  name,  sizeof(name)))  urldecode_inplace(name);
    int port = atoi(ports);

    int rc = audio_icepush_start(host, port, mount, pass, name);
    if (rc == 0) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "host", host);
        cJSON_AddNumberToObject(o, "port", port > 0 ? port : 8000);
        cJSON_AddStringToObject(o, "mount", mount);
        cJSON_AddStringToObject(o, "pass", pass);
        cJSON_AddStringToObject(o, "name", name);
        char *txt = cJSON_Print(o);
        cJSON_Delete(o);
        if (txt) { writeJSONFile(ICE_CFG_PATH, txt); free(txt); }
        return send_json(req, "{\"ok\":true}");
    }
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"why\":\"%s\"}",
             rc == -1 ? "already running" : rc == -2 ? "need host+mount" : "task create failed");
    return send_json(req, buf);
}

static esp_err_t ice_stop_handler(httpd_req_t *req)
{
    audio_icepush_stop();
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t ice_state_handler(httpd_req_t *req)
{
    char host[64] = "", mount[48] = "", pass[48] = "", name[48] = "";
    int port = 8000;
    cJSON *saved = readJSONFileAsCJSON(ICE_CFG_PATH);
    if (saved) {
        cJSON *j;
        if ((j = cJSON_GetObjectItemCaseSensitive(saved, "host"))  && cJSON_IsString(j)) strlcpy(host,  j->valuestring, sizeof(host));
        if ((j = cJSON_GetObjectItemCaseSensitive(saved, "port"))  && cJSON_IsNumber(j)) port = j->valueint;
        if ((j = cJSON_GetObjectItemCaseSensitive(saved, "mount")) && cJSON_IsString(j)) strlcpy(mount, j->valuestring, sizeof(mount));
        if ((j = cJSON_GetObjectItemCaseSensitive(saved, "pass"))  && cJSON_IsString(j)) strlcpy(pass,  j->valuestring, sizeof(pass));
        if ((j = cJSON_GetObjectItemCaseSensitive(saved, "name"))  && cJSON_IsString(j)) strlcpy(name,  j->valuestring, sizeof(name));
        cJSON_Delete(saved);
    }
    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"run\":%s,\"up\":%s,\"err\":\"%s\",\"retries\":%u,"
             "\"cfg\":{\"host\":\"%s\",\"port\":%d,\"mount\":\"%s\",\"pass\":\"%s\",\"name\":\"%s\"}}",
             audio_icepush_running() ? "true" : "false",
             audio_icepush_connected() ? "true" : "false",
             audio_icepush_err(), (unsigned)audio_icepush_retries(),
             host, port, mount, pass, name);
    return send_json(req, buf);
}

// ─── server lifecycle ────────────────────────────────────────────────────────

// ─── POST /ota — stream a new firmware image into the inactive OTA slot ──────
// (esptool-free updates over WiFi; reboots into the new image on success)
//   curl -X POST --data-binary @build/ctag-straempler.bin http://<ip>/ota
static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL);
    if (!upd) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA slot"); return ESP_FAIL; }
    if (req->content_len < 0x10000) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image too small"); return ESP_FAIL; }
    esp_ota_handle_t h = 0;
    if (esp_ota_begin(upd, OTA_SIZE_UNKNOWN, &h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed"); return ESP_FAIL;
    }
    // Silence the rack for the whole flash (Arlo 2026-07-25): streaming the image
    // and the reboot itself otherwise dump garbage into the mix. Slewed, so this
    // is a fade. Every FAILURE path below re-opens it; the success path
    // deliberately does NOT -- it stays muted straight through esp_restart().
    audio_output_mute(true);
    char *buf = malloc(4096);
    if (!buf) { esp_ota_abort(h); audio_output_mute(false); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int remaining = req->content_len, total = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining > 4096 ? 4096 : remaining);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf); esp_ota_abort(h); audio_output_mute(false);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed"); return ESP_FAIL;
        }
        if (esp_ota_write(h, buf, r) != ESP_OK) {
            free(buf); esp_ota_abort(h); audio_output_mute(false);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write failed"); return ESP_FAIL;
        }
        total += r; remaining -= r;
    }
    free(buf);
    esp_err_t err = esp_ota_end(h);
    if (err != ESP_OK) {
        audio_output_mute(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            err == ESP_ERR_OTA_VALIDATE_FAILED ? "image validation failed" : "ota_end failed");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(upd) != ESP_OK) {
        audio_output_mute(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot failed"); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: %d bytes -> %s, rebooting", total, upd->label);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    // flush the active machine's live state to AUTOSAVE.JSN before the reboot:
    // an OTA reboot is immediate, so any edits since the last debounced save
    // (knob tweaks never arm autosave at all) would otherwise be lost across the
    // flash — "drums lost settings between flashes". Runs on the UI task.
    if (ui_ev_queue) {
        ui_ev_ts_t ae = { .event = EV_AUTOSAVE, .event_data = NULL };
        xQueueSend(ui_ev_queue, &ae, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(800));   // let the response flush AND the autosave land
    esp_restart();
    return ESP_OK;
}

// POST /reboot — soft restart. Exists for ONE recurring failure: a long OTA
// session fragments internal RAM until esp_ota_begin cannot get its contiguous
// buffer ("ota_begin failed"), and the only recovery was walking to the module
// and pulling power. esp_restart() reinitialises the heap, so this clears it
// remotely. Reuses the OTA handler's shutdown sequence exactly (queue
// EV_AUTOSAVE, let it land, then restart) so machine state survives. REFUSES
// while recording — a reboot destroys the PSRAM take with no trace. Ungated like
// /ota: gating a reboot more strictly than an endpoint that can replace the
// firmware would be incoherent, and this is a LAN device. It cannot rescue a
// genuinely WEDGED device — if the httpd task is starved, nothing answers this
// either; it is for the fragmentation case.
static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (recording_is_active()) {            // a reboot would destroy the take
        httpd_resp_set_status(req, "409 Conflict");   // IDF 4.3 has no HTTPD_409_* enum
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"recording\"}");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "soft reboot requested");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    if (ui_ev_queue) {                       // flush live machine state first
        ui_ev_ts_t ae = { .event = EV_AUTOSAVE, .event_data = NULL };
        xQueueSend(ui_ev_queue, &ae, 0);
    }
    audio_output_mute(true);                 // don't dump garbage into the rack
    vTaskDelay(pdMS_TO_TICKS(800));          // response flush + autosave land
    esp_restart();
    return ESP_OK;
}

// GET /ota/state — running partition + which slot the next update lands in
static esp_err_t ota_state_handler(httpd_req_t *req)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *nxt = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *desc = esp_ota_get_app_description();
    char s[192];
    snprintf(s, sizeof(s), "{\"running\":\"%s\",\"next\":\"%s\",\"ver\":\"%s\"}",
             run ? run->label : "?", nxt ? nxt->label : "?", desc ? desc->version : "?");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, s);
}

// GET /peaks?name=<id>[&n=<cols>] — a waveform THUMBNAIL: n peak columns
// (0..255), one raw byte each. Stride-reads a small window at n evenly spaced
// points, so the cost is independent of file size (a 24 MB take is as cheap as a
// 100 KB one). Feeds the web file manager's inline waveforms. Bursts hold sd_lock
// per-read (never across the whole scan) so audio SD reads aren't starved.
static esp_err_t peaks_handler(httpd_req_t *req)
{
    char name[32];
    if (!get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }
    char ns[8];
    int n = 48;
    if (get_query_param(req, "n", ns, sizeof(ns))) n = atoi(ns);
    if (n < 8) n = 8; else if (n > 128) n = 128;

    char path[72];
    sample_resolve(name, path, sizeof(path));

    sd_lock_take();
    FILE *f = fopen(path, "rb");
    sampfile_t sf;
    int ok = (f && sampfile_probe(f, &sf) == 0 && sf.frames > 0);
    sd_lock_give();
    if (!ok) {
        if (f) fclose(f);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    static uint8_t  peaks[128];        // handlers are serialized (single httpd task)
    static int16_t  win[256 * 2];      // 256-frame stereo staging window
    const uint32_t  wf = 256;
    uint32_t frames = sf.frames;
    for (int c = 0; c < n; c++) {
        uint32_t start = (uint32_t)((uint64_t)c * frames / (uint32_t)n);
        sd_lock_take();
        fseek(f, sf_seek_pos(&sf, start), SEEK_SET);
        size_t got = sampfile_read(f, &sf, win, wf);
        sd_lock_give();
        int peak = 0;
        for (size_t i = 0; i < got; i++) {
            int a = win[2 * i];     if (a < 0) a = -a; if (a > peak) peak = a;
            int b = win[2 * i + 1]; if (b < 0) b = -b; if (b > peak) peak = b;
        }
        peaks[c] = (uint8_t)(((peak > 32767 ? 32767 : peak) * 255) / 32767);
    }
    sd_lock_take();
    fclose(f);
    sd_lock_give();

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, (const char *)peaks, n);
    return ESP_OK;
}

static httpd_uri_t uris[] = {
    { .uri = "/",           .method = HTTP_GET,    .handler = landing_handler },
    { .uri = "/ota",        .method = HTTP_POST,   .handler = ota_post_handler },
    { .uri = "/ota/state",  .method = HTTP_GET,    .handler = ota_state_handler },
    { .uri = "/reboot",     .method = HTTP_POST,   .handler = reboot_post_handler },
    { .uri = "/sysinfo",    .method = HTTP_GET,    .handler = sysinfo_get_handler },
    { .uri = "/files",      .method = HTTP_GET,    .handler = files_get_handler },
    { .uri = "/files",      .method = HTTP_DELETE, .handler = files_delete_handler },
    { .uri = "/files/rename", .method = HTTP_POST, .handler = files_rename_handler },
    { .uri = "/files/move",   .method = HTTP_POST, .handler = files_move_handler },
    { .uri = "/files/raw",  .method = HTTP_GET,    .handler = files_raw_handler },
    { .uri = "/peaks",      .method = HTTP_GET,    .handler = peaks_handler },
    { .uri = "/screenshot", .method = HTTP_GET,    .handler = screenshot_get_handler },
    { .uri = "/import",     .method = HTTP_POST,   .handler = import_post_handler },
    { .uri = "/import",     .method = HTTP_GET,    .handler = import_get_handler },
    { .uri = "/settings",   .method = HTTP_GET,    .handler = settings_get_handler },
    { .uri = "/settings",   .method = HTTP_POST,   .handler = settings_post_handler },
    { .uri = "/status",     .method = HTTP_GET,    .handler = status_get_handler },
    { .uri = "/drop_sample",.method = HTTP_PUT,    .handler = drop_sample_put_handler },
    { .uri = "/bootlogo",   .method = HTTP_PUT,    .handler = bootlogo_put_handler },
    { .uri = "/trk/upload", .method = HTTP_PUT,    .handler = mod_upload_handler },
    { .uri = "/drop_ot",    .method = HTTP_PUT,    .handler = drop_ot_put_handler },
    { .uri = "/trk/list",   .method = HTTP_GET,    .handler = mod_list_handler },
    { .uri = "/trk/get",    .method = HTTP_GET,    .handler = mod_get_handler },
    { .uri = "/trk/delete", .method = HTTP_DELETE, .handler = mod_delete_handler },
    { .uri = "/remote/event",  .method = HTTP_POST, .handler = remote_event_handler },
    { .uri = "/remote/trig",   .method = HTTP_POST, .handler = remote_trig_handler },
    { .uri = "/remote/cv",     .method = HTTP_POST, .handler = remote_cv_handler },
    { .uri = "/bounce/start",  .method = HTTP_POST, .handler = bounce_start_handler },
    { .uri = "/bounce/stop",   .method = HTTP_POST, .handler = bounce_stop_handler },
    { .uri = "/bounce/state",  .method = HTTP_GET,  .handler = bounce_state_handler },
    { .uri = "/bcast/state",   .method = HTTP_GET,  .handler = bcast_state_handler },
    { .uri = "/bcast/enable",  .method = HTTP_GET,  .handler = bcast_enable_handler },
    { .uri = "/ws/midi",       .method = HTTP_GET,  .handler = midi_ws_handler, .is_websocket = true },
    { .uri = "/midi/on",       .method = HTTP_POST, .handler = midi_post_handler },
    { .uri = "/midi/off",      .method = HTTP_POST, .handler = midi_post_handler },
    { .uri = "/midi/alloff",   .method = HTTP_POST, .handler = midi_post_handler },
    { .uri = "/ice/start",     .method = HTTP_POST, .handler = ice_start_handler },
    { .uri = "/ice/stop",      .method = HTTP_POST, .handler = ice_stop_handler },
    { .uri = "/ice/state",     .method = HTTP_GET,  .handler = ice_state_handler },
    { .uri = "/blisten",       .method = HTTP_POST, .handler = blisten_post_handler },
    { .uri = "/tuner/enable",  .method = HTTP_GET,  .handler = tuner_enable_handler },
    { .uri = "/remote/machine",.method = HTTP_POST, .handler = remote_machine_handler },
    { .uri = "/remote/params", .method = HTTP_GET,  .handler = remote_params_get_handler },
    { .uri = "/remote/params", .method = HTTP_POST, .handler = remote_params_post_handler },
};
#define N_URIS (sizeof(uris)/sizeof(uris[0]))

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size      = 4096 * 2;
    config.core_id         = 0;
    config.task_priority   = 5;
    config.max_uri_handlers = N_URIS + 2 + MAX_MACHINE_URIS;
    // abandoned sockets (aborted polls, vanished clients) used to pile up
    // until all ~7 session slots were dead and every new connection got RST
    // ("REST died" while the firmware ran fine). Purge the LRU session
    // instead of refusing the connection.
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting server on port %d", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server");
        return NULL;
    }
    for (int i = 0; i < (int)N_URIS; i++)
        httpd_register_uri_handler(server, &uris[i]);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);
    machine_web_apply(web_machine);   // machine activated before the server? apply now
    return server;
}

void startRestAPI(xQueueHandle queueui)
{
    ui_ev_queue = queueui;
    // boot-time teleremote flag (menu_config lives above us in the dep graph,
    // so read the config file directly)
    cJSON *cfg = readJSONFileAsCJSON("/sdcard/CONFIG.JSN");
    if (cfg) {
        cJSON *settings = cJSON_GetObjectItemCaseSensitive(cfg, "settings");
        cJSON *r = settings ? cJSON_GetObjectItemCaseSensitive(settings, "remote") : NULL;
        if (r && cJSON_IsNumber(r)) s_remote_on = r->valueint ? 1 : 0;
        cJSON_Delete(cfg);
    }
    start_webserver();
    machine_set_web_cb(machine_web_apply);
}

void stopRestAPI(void)
{
    machine_set_web_cb(NULL);
    n_machine_uris = 0;
    httpd_stop(server);
    server = NULL;
}
