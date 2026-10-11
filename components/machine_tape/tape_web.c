// Tape's web route: POST /tape/load?name=<id> — the web file browser's "load to
// machine". The load itself streams the take from the card (seconds), so the
// handler only queues it; the Tape UI tick runs it (tape_web_load_service).
#include <string.h>
#include <stdlib.h>
#include <esp_http_server.h>
#include "esp_log.h"
#include "sampfile.h"
#include "urldec.h"
#include "tape_priv.h"

static const char *TAG = "TAPE";

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *s)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, s);
}

static esp_err_t load_handler(httpd_req_t *req)
{
    char name[SAMPLE_ID_LEN] = "";
    size_t ql = httpd_req_get_url_query_len(req) + 1;
    char *q = ql > 1 ? malloc(ql) : NULL;
    bool ok = q && httpd_req_get_url_query_str(req, q, ql) == ESP_OK &&
              httpd_query_key_value(q, "name", name, sizeof(name)) == ESP_OK && name[0];
    free(q);
    if (ok) urldec_inplace(name);
    if (!ok) return send_json(req, "400 Bad Request", "{\"error\":\"need ?name=\"}");
    // the panel's rule: a load replaces the tape, so it waits for a stop
    if (tp.playing || tp.recording) return send_json(req, "409 Conflict", "{\"error\":\"stop the tape first\"}");
    // one request at a time: the UI task may be copying the last name out
    if (tp.web_load_req) return send_json(req, "409 Conflict", "{\"error\":\"a load is already queued\"}");
    strlcpy(tp.web_load, name, sizeof(tp.web_load));
    tp.web_load_req = true;
    return send_json(req, "200 OK", "{\"ok\":true,\"queued\":true}");   // loads on the next UI tick
}

// UI task (the Live page's slow tick, or Tape's other pages): run a queued web
// load. True when a load ran, so the Live page repaints.
bool tape_web_load_service(void)
{
    if (!tp.web_load_req) return false;
    // copy out BEFORE clearing the flag: tape_load reads the name again after
    // its seconds-long stream, and a new request may refill tp.web_load then
    char nm[SAMPLE_ID_LEN];
    strlcpy(nm, tp.web_load, sizeof(nm));
    tp.web_load_req = false;
    if (tp.playing || tp.recording) { ESP_LOGW(TAG, "web load %s dropped: tape rolling", nm); return false; }
    int r = tape_load(nm);
    if (r != 0) ESP_LOGE(TAG, "web load %s failed (%d)", nm, r);
    return r == 0;
}

const httpd_uri_t tape_web_uris[] = {
    { .uri = "/tape/load", .method = HTTP_POST, .handler = load_handler },
};
