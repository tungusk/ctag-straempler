#include "rest-api.h"
#include "index.html.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include <esp_http_server.h>
#include "ui_events.h"
#include "string_tools.h"
#include "fileio.h"
#include "audio.h"
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
    // cJSON handles string escaping — descriptions/tags may contain quotes
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "files");

    DIR *d = opendir("/sdcard/usr");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_type != DT_REG) continue;
            int len = strlen(ent->d_name);
            if (len < 5) continue;
            // only list .JSN sidecars — each represents a complete sample
            if (strcasecmp(ent->d_name + len - 4, ".JSN") != 0) continue;

            char jsn_path[280];
            snprintf(jsn_path, sizeof(jsn_path), "/sdcard/usr/%s", ent->d_name);
            cJSON *meta = readJSONFileAsCJSON(jsn_path);
            if (!meta) continue;

            // derive RAW path and size
            char raw_path[280];
            char id[260] = {0};
            strncpy(id, ent->d_name, len - 4);  // strip .JSN
            snprintf(raw_path, sizeof(raw_path), "/sdcard/usr/%s.RAW", id);
            struct stat st;
            long fsize = (stat(raw_path, &st) == 0) ? st.st_size : 0;

            const char *desc = "";
            const char *tags = "";
            cJSON *j;
            if ((j = cJSON_GetObjectItem(meta, "description")) && j->valuestring) desc = j->valuestring;
            if ((j = cJSON_GetObjectItem(meta, "tags_s")) && j->valuestring) tags = j->valuestring;

            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", id);
            cJSON_AddStringToObject(o, "description", desc);
            cJSON_AddStringToObject(o, "tags", tags);
            cJSON_AddNumberToObject(o, "size", fsize);
            cJSON_AddItemToArray(arr, o);
            cJSON_Delete(meta);
        }
        closedir(d);
    }

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) { send_json(req, s); free(s); }
    else send_json(req, "{\"files\":[]}");
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
    snprintf(path, sizeof(path), "/sdcard/usr/%s.RAW", name);
    remove(path);
    snprintf(path, sizeof(path), "/sdcard/usr/%s.JSN", name);
    remove(path);

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
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    struct stat st;
    stat(path, &st);
    char len_str[16];
    snprintf(len_str, sizeof(len_str), "%ld", (long)st.st_size);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Length", len_str);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char *buf = malloc(STREAM_CHUNK);
    if (!buf) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }

    int n;
    while ((n = fread(buf, 1, STREAM_CHUNK, f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
        // yield the SD bus between chunks so audio file-reader refills aren't starved
        vTaskDelay(1);
    }
    httpd_resp_send_chunk(req, NULL, 0);

    fclose(f);
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

    // build compact JSON by hand to avoid cJSON overhead in hot path
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"recording\":%s,\"v0\":\"%s\",\"v1\":\"%s\","
        "\"cv\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
        rec ? "true" : "false",
        st.v0, st.v1,
        st.cv[0], st.cv[1], st.cv[2], st.cv[3],
        st.cv[4], st.cv[5], st.cv[6], st.cv[7]);
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
    FRESULT fr = f_open(&raw_file, file_name, FA_CREATE_ALWAYS | FA_WRITE);
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
        f_write(&raw_file, zeros, pad, &bw);
    }
    remaining = req->content_len;

    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, 4096))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            f_close(&raw_file); f_unlink(file_name); free(buf); cJSON_Delete(root);
            return ESP_FAIL;
        }
        total += ret; remaining -= ret;
        if (ret > 0) f_write(&raw_file, buf, ret, &bw);
        ev.event = EV_DECODING_PROGRESS;
        ev.event_data = (void *)(total / (file_len_d100 ? file_len_d100 : 1));
        xQueueSend(ui_ev_queue, &ev, portMAX_DELAY);
    }

    cJSON_AddStringToObject(root, "username", "myself");
    cJSON_AddStringToObject(root, "url", "local");
    cJSON_AddStringToObject(root, "license", "own license");
    writeJSONFile(file_name_jsn, cJSON_Print(root));

    f_close(&raw_file);
    free(buf);
    cJSON_Delete(root);

    httpd_resp_send(req, NULL, 0);
    ev.event = EV_DECODING_DONE;
    xQueueSend(ui_ev_queue, &ev, portMAX_DELAY);
    return ESP_OK;
}

// ─── server lifecycle ────────────────────────────────────────────────────────

static httpd_uri_t uris[] = {
    { .uri = "/",           .method = HTTP_GET,    .handler = landing_handler },
    { .uri = "/files",      .method = HTTP_GET,    .handler = files_get_handler },
    { .uri = "/files",      .method = HTTP_DELETE, .handler = files_delete_handler },
    { .uri = "/files/raw",  .method = HTTP_GET,    .handler = files_raw_handler },
    { .uri = "/settings",   .method = HTTP_GET,    .handler = settings_get_handler },
    { .uri = "/settings",   .method = HTTP_POST,   .handler = settings_post_handler },
    { .uri = "/status",     .method = HTTP_GET,    .handler = status_get_handler },
    { .uri = "/drop_sample",.method = HTTP_PUT,    .handler = drop_sample_put_handler },
};
#define N_URIS (sizeof(uris)/sizeof(uris[0]))

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size      = 4096 * 2;
    config.core_id         = 0;
    config.task_priority   = 5;
    config.max_uri_handlers = N_URIS + 2;

    ESP_LOGI(TAG, "Starting server on port %d", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server");
        return NULL;
    }
    for (int i = 0; i < (int)N_URIS; i++)
        httpd_register_uri_handler(server, &uris[i]);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);
    return server;
}

void startRestAPI(xQueueHandle queueui)
{
    ui_ev_queue = queueui;
    start_webserver();
}

// These are now no-ops — the full API is always registered at startup.
void setRestAPIUserReceiveOn(void)  {}
void setRestAPIUserReceiveOff(void) {}

void stopRestAPI(void)
{
    httpd_stop(server);
}
