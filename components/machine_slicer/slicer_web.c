// Slicer machine web endpoint — registered by the core httpd while the
// slicer is active (machine_ui_t.web_uris):
//   GET /slicer/ot   download the CURRENT slices as an Octatrack .ot file
//                    (832 bytes; 404 no sample, 409 more than 64 slices)
#include <stdio.h>
#include <string.h>
#include <esp_http_server.h>
#include "cJSON.h"
#include "fileio.h"
#include "slicer_priv.h"
#include "sampfile.h"

static float sidecar_bpm(const char *name)
{
    char jp[64];
    sample_resolve_aux(name, ".JSN", jp, sizeof(jp));
    cJSON *root = readJSONFileAsCJSON(jp);
    float bpm = 0;
    if (root) {
        cJSON *j = cJSON_GetObjectItemCaseSensitive(root, "bpm");
        if (j && cJSON_IsNumber(j)) bpm = (float)j->valuedouble;
        cJSON_Delete(root);
    }
    return bpm;   // 0 = unknown -> builder defaults to 120
}

static esp_err_t slicer_ot_get_handler(httpd_req_t *req)
{
    if (sl.len == 0 || !sl.sample[0]) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No sample loaded");
        return ESP_FAIL;
    }
    if (sl.n_slices > SL_OT_SLICES) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Octatrack max is 64 slices (have %d)", sl.n_slices);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }
    uint8_t buf[832];
    if (slicer_build_ot(buf, sidecar_bpm(sl.sample)) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Build failed");
        return ESP_FAIL;
    }
    char cd[64];
    snprintf(cd, sizeof(cd), "attachment; filename=\"%s.OT\"", sl.sample);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, (const char *)buf, sizeof(buf));
    return ESP_OK;
}

const httpd_uri_t slicer_web_uris[] = {
    { .uri = "/slicer/ot", .method = HTTP_GET, .handler = slicer_ot_get_handler },
};
