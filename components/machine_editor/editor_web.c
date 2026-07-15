// Editor REST endpoints (served only while the Editor machine is active):
//   POST /edit/apply?name=<id>&op=<0..4>[&param=<f>]   kick an op
//   GET  /edit/state                                    JSON status + op list
#include <string.h>
#include <stdlib.h>
#include <esp_http_server.h>
#include "cJSON.h"
#include "editor_priv.h"

static bool qparam(httpd_req_t *req, const char *key, char *out, size_t n)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) return false;
    char *q = malloc(qlen);
    if (!q) return false;
    bool ok = false;
    if (httpd_req_get_url_query_str(req, q, qlen) == ESP_OK)
        ok = (httpd_query_key_value(q, key, out, n) == ESP_OK);
    free(q);
    return ok;
}

static esp_err_t send_json(httpd_req_t *req, const char *s)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, s);
}

static esp_err_t apply_handler(httpd_req_t *req)
{
    char name[ED_NAME_LEN], ops[8], ps[16];
    if (!qparam(req, "name", name, sizeof(name)) || !qparam(req, "op", ops, sizeof(ops))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need ?name=&op=");
        return ESP_FAIL;
    }
    float param = qparam(req, "param", ps, sizeof(ps)) ? (float)atof(ps) : 0.0f;
    editor_apply(name, atoi(ops), param);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t state_handler(httpd_req_t *req)
{
    static const char *const st[] = { "idle", "running", "done", "error" };
    cJSON *o = cJSON_CreateObject();
    int s = ed.state; if (s < 0 || s > 3) s = 0;
    cJSON_AddStringToObject(o, "state", st[s]);
    cJSON_AddNumberToObject(o, "progress", ed.progress);
    cJSON_AddStringToObject(o, "src", ed.src);
    cJSON_AddStringToObject(o, "out", ed.out);
    if (ed.op >= 0 && ed.op < OP_N) cJSON_AddStringToObject(o, "op", ed_op_names[ed.op]);
    if (ed.err[0]) cJSON_AddStringToObject(o, "err", ed.err);
    cJSON *arr = cJSON_AddArrayToObject(o, "ops");
    for (int i = 0; i < OP_N; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(ed_op_names[i]));
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    esp_err_t rc = send_json(req, js ? js : "{}");
    free(js);
    return rc;
}

const httpd_uri_t editor_web_uris[] = {
    { .uri = "/edit/apply", .method = HTTP_POST, .handler = apply_handler },
    { .uri = "/edit/state", .method = HTTP_GET,  .handler = state_handler },
};
const int editor_web_n_uris = sizeof(editor_web_uris) / sizeof(editor_web_uris[0]);
