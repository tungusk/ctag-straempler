// Looper machine web endpoint — registered by the core httpd while the
// looper is active (machine_ui_t.web_uris):
//   POST /looper/save[?trk=1..4]   write that track (default: the selected
//   one) to usr/LOOPS as WAV + sidecar — the same action as the Setup row,
//   callable deterministically from the web/scripts. 200 {"saved":"..."} on
//   success, 409 when the track is empty.
#include <stdio.h>
#include <stdlib.h>
#include <esp_http_server.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "looper_priv.h"

// The save runs on a WORKER task, never the httpd task: the FAT write path
// (newlib -> VFS -> f_write -> cluster allocation -> sdmmc) is deep, and on
// the 8 KB httpd stack it smashed newlib's FILE internals — captured panic:
// LoadStoreError in fwrite/memmove with a ROM address as the stdio buffer.
// Same house rule as sampimport (rest-api.c: heavy jobs get ~20 KB tasks).
static volatile int s_sv_done = 1, s_sv_rc = -1;
static int s_sv_trk;

static void save_worker(void *pv)
{
    s_sv_rc = looper_save_track(s_sv_trk);
    s_sv_done = 1;
    vTaskDelete(NULL);
}

static esp_err_t looper_save_post_handler(httpd_req_t *req)
{
    int trk = lp.sel;
    char q[8];
    size_t ql = httpd_req_get_url_query_len(req) + 1;
    if (ql > 1 && ql < 32) {
        char qs[32];
        if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK &&
            httpd_query_key_value(qs, "trk", q, sizeof(q)) == ESP_OK) {
            int t = atoi(q);
            if (t >= 1 && t <= LP_TRACKS) trk = t - 1;
        }
    }
    if (!s_sv_done) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "save already running");
        return ESP_FAIL;
    }
    s_sv_done = 0;
    s_sv_trk = trk;
    if (xTaskCreate(save_worker, "lp_save", 12288, NULL, 5, NULL) != pdPASS) {
        s_sv_done = 1;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "task create failed");
        return ESP_FAIL;
    }
    for (int i = 0; i < 600 && !s_sv_done; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (!s_sv_done) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save timed out");
        return ESP_FAIL;
    }
    if (s_sv_rc != 0) {
        char why[96];
        snprintf(why, sizeof(why),
                 "save failed: trk=%d state=%d len=%lu buf=%s",
                 trk + 1, lp.tr[trk].state, (unsigned long)lp.tr[trk].len,
                 lp.tr[trk].buf ? "ok" : "NULL");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char out[48];
    snprintf(out, sizeof(out), "{\"saved\":\"track %d\"}", trk + 1);
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

const httpd_uri_t looper_web_uris[] = {
    { .uri = "/looper/save", .method = HTTP_POST, .handler = looper_save_post_handler },
};
