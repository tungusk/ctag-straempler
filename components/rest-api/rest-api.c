#include "rest-api.h"
#include "index.html.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include <esp_http_server.h>
#include "ui_events.h"
#include "string_tools.h"
#include "fileio.h"
#include "sd_lock.h"
#include "audio.h"
#include "machine.h"
#include "recording.h"
#include "wifi.h"
#include "freesound.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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
    httpd_resp_send(req, index_html, index_html_len);
    return ESP_OK;
}

// ─── GET /files ──────────────────────────────────────────────────────────────

static esp_err_t files_get_handler(httpd_req_t *req)
{
    // STREAMED response. Internal RAM on this board is tight enough that
    // building the whole file array + printing it in one shot OOMs (the list
    // came back empty). Instead we emit one small object per file as we walk
    // the directory, so peak memory stays flat no matter how many files exist.
    // We also skip reading each .JSN sidecar: names come free from readdir and
    // sizes from stat, so the listing never touches the failing per-file reads
    // that were dragging the handler and tripping browser socket resets.
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send_chunk(req, "{\"files\":[", HTTPD_RESP_USE_STRLEN);

    sd_lock_take();
    DIR *d = opendir("/sdcard/usr");
    sd_lock_give();
    bool first = true;
    if (d) {
        for (;;) {
            // grab the SD bus only for the readdir step, release between entries
            sd_lock_take();
            struct dirent *ent = readdir(d);
            sd_lock_give();
            if (ent == NULL) break;
            if (ent->d_type != DT_REG) continue;
            int len = strlen(ent->d_name);
            if (len < 5) continue;
            // only list .JSN sidecars — each represents a complete sample
            if (strcasecmp(ent->d_name + len - 4, ".JSN") != 0) continue;

            // id = sidecar name without the .JSN extension
            char id[260] = {0};
            strncpy(id, ent->d_name, len - 4);

            // RAW size via stat — light, no file open needed
            char raw_path[280];
            snprintf(raw_path, sizeof(raw_path), "/sdcard/usr/%s.RAW", id);
            struct stat st;
            sd_lock_take();
            long fsize = (stat(raw_path, &st) == 0) ? st.st_size : 0;
            sd_lock_give();

            // build ONE tiny object (cJSON just for correct string escaping),
            // print it small, stream it, free it — flat memory footprint
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", id);
            cJSON_AddStringToObject(o, "description", "");
            cJSON_AddStringToObject(o, "tags", "");
            cJSON_AddNumberToObject(o, "size", fsize);
            char *os = cJSON_PrintUnformatted(o);
            cJSON_Delete(o);
            if (os) {
                if (!first) httpd_resp_send_chunk(req, ",", 1);
                httpd_resp_send_chunk(req, os, HTTPD_RESP_USE_STRLEN);
                free(os);
                first = false;
            }
        }
        sd_lock_take();
        closedir(d);
        sd_lock_give();
    }

    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
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
    sd_lock_take();
    snprintf(path, sizeof(path), "/sdcard/usr/%s.RAW", name);
    remove(path);
    snprintf(path, sizeof(path), "/sdcard/usr/%s.JSN", name);
    remove(path);
    sd_lock_give();

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{}");
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
    snprintf(path, sizeof(path), "/sdcard/usr/%s.RAW", name);
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
        httpd_resp_send_chunk(req, buf, n);
        vTaskDelay(1);
    }
    httpd_resp_send_chunk(req, NULL, 0);

    sd_lock_take();
    fclose(f);
    sd_lock_give();
    free(buf);
    return ESP_OK;
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

static esp_err_t status_get_handler(httpd_req_t *req)
{
    audio_status_t st;
    audio_get_status(&st);
    bool rec = recording_is_active();
    const machine_t *m = machine_active();

    // build compact JSON by hand to avoid cJSON overhead in hot path
    char buf[288];
    int n = snprintf(buf, sizeof(buf),
        "{\"machine\":\"%s\",\"recording\":%s,\"v0\":\"%s\",\"v1\":\"%s\","
        "\"cv\":[%u,%u,%u,%u,%u,%u,%u,%u],\"trig\":%u}",
        m ? m->name : "",
        rec ? "true" : "false",
        st.v0, st.v1,
        st.cv[0], st.cv[1], st.cv[2], st.cv[3],
        st.cv[4], st.cv[5], st.cv[6], st.cv[7],
        st.trig);
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
    if (req->content_len % 4 != 0) {
        int pad = 4 - (req->content_len % 4);
        const char zeros[3] = {0};
        sd_lock_take();
        f_write(&raw_file, zeros, pad, &bw);
        sd_lock_give();
    }
    remaining = req->content_len;

    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, 4096))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            sd_lock_take();
            f_close(&raw_file); f_unlink(file_name);
            sd_lock_give();
            free(buf); cJSON_Delete(root);
            return ESP_FAIL;
        }
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

    cJSON_AddStringToObject(root, "username", "myself");
    cJSON_AddStringToObject(root, "url", "local");
    cJSON_AddStringToObject(root, "license", "own license");
    writeJSONFile(file_name_jsn, cJSON_Print(root));

    sd_lock_take();
    f_close(&raw_file);
    sd_lock_give();
    free(buf);
    cJSON_Delete(root);

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

    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"ip\":\"%s\",\"free\":%llu,\"total\":%llu,\"remote\":%d,\"machines\":[%s]}",
             ip, (unsigned long long)freeb, (unsigned long long)totb,
             s_remote_on ? 1 : 0, machines);
    send_json(req, buf);
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

// ─── server lifecycle ────────────────────────────────────────────────────────

static httpd_uri_t uris[] = {
    { .uri = "/",           .method = HTTP_GET,    .handler = landing_handler },
    { .uri = "/sysinfo",    .method = HTTP_GET,    .handler = sysinfo_get_handler },
    { .uri = "/files",      .method = HTTP_GET,    .handler = files_get_handler },
    { .uri = "/files",      .method = HTTP_DELETE, .handler = files_delete_handler },
    { .uri = "/files/raw",  .method = HTTP_GET,    .handler = files_raw_handler },
    { .uri = "/settings",   .method = HTTP_GET,    .handler = settings_get_handler },
    { .uri = "/settings",   .method = HTTP_POST,   .handler = settings_post_handler },
    { .uri = "/status",     .method = HTTP_GET,    .handler = status_get_handler },
    { .uri = "/drop_sample",.method = HTTP_PUT,    .handler = drop_sample_put_handler },
    { .uri = "/remote/event",  .method = HTTP_POST, .handler = remote_event_handler },
    { .uri = "/remote/trig",   .method = HTTP_POST, .handler = remote_trig_handler },
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
