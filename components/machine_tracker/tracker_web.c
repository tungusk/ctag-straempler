// Tracker web upload — PUT /trk/upload stores a module file verbatim to
// usr/<NAME>.<EXT>. Registered via the machine web-URI hook, so it exists only
// while the Tracker machine is active. Modeled on rest-api.c's /drop_sample
// receive loop (4 KB chunks, sd_lock per write burst, unlink on abort) but
// writes raw bytes (no RAW padding, no JSN sidecar).
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <esp_http_server.h>
#include "esp_log.h"
#include "ff.h"
#include "sd_lock.h"
#include "string_tools.h"
#include "tracker_priv.h"

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

static const char *TAG = "TRK-WEB";

// accepted upload extensions (upper-cased on disk). Superset covers every
// libxmp loader we ship; the browser filter (tracker.c) shows the same set.
static const char *const UP_EXTS[] = {
    "MOD","XM","IT","S3M","669","MTM","OKT","ULT","FAR","MED","DBM","AMF",
    "PTM","STM","DMF","GDM","IMF","LIQ","MDL","PT3","OXM","DIGI","EMOD",
};
#define N_UP_EXTS (int)(sizeof(UP_EXTS)/sizeof(UP_EXTS[0]))

static bool ext_ok(const char *e)
{
    for (int i = 0; i < N_UP_EXTS; i++) if (strcasecmp(e, UP_EXTS[i]) == 0) return true;
    return false;
}

static esp_err_t trk_upload_handler(httpd_req_t *req)
{
    char name[16] = "", ext[8] = "";

    // Name header → 8.3 base name (FatFS is 8.3-only on this card)
    size_t nl = httpd_req_get_hdr_value_len(req, "Name") + 1;
    if (nl > 1) {
        char *b = malloc(nl);
        if (httpd_req_get_hdr_value_str(req, "Name", b, nl) == ESP_OK) {
            cleanStringSpace(b);
            b[8] = 0;                       // 8.3: clamp base to 8 chars
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
    if (!name[0] || !ext_ok(ext)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad name/ext");
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > TRK_MAX_FILE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Module too big or empty");
        return ESP_FAIL;
    }

    char path[40];
    // uppercase the extension on disk; bare FatFS path (no /sdcard prefix)
    char up[8]; strlcpy(up, ext, sizeof(up));
    for (char *p = up; *p; p++) *p = toupper((unsigned char)*p);
    snprintf(path, sizeof(path), "/usr/%s.%s", name, up);

    FIL f;
    sd_lock_take();
    FRESULT fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
    sd_lock_give();
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "f_open %s failed (%d)", path, fr);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD open failed");
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    int remaining = req->content_len, ret;
    UINT bw;
    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, 4096))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            sd_lock_take(); f_close(&f); f_unlink(path); sd_lock_give();
            free(buf);
            return ESP_FAIL;
        }
        remaining -= ret;
        sd_lock_take();
        f_write(&f, buf, ret, &bw);
        sd_lock_give();
    }
    sd_lock_take(); f_close(&f); sd_lock_give();
    free(buf);

    ESP_LOGI(TAG, "uploaded %s (%d bytes)", path, (int)req->content_len);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

const httpd_uri_t tracker_web_uris[] = {
    { .uri = "/trk/upload", .method = HTTP_PUT, .handler = trk_upload_handler },
};
const int tracker_web_n_uris = sizeof(tracker_web_uris) / sizeof(tracker_web_uris[0]);
