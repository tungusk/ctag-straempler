// Freesound machine web endpoints — registered by the core httpd while this
// machine is active (machine_ui_t.web_uris):
//   GET  /fs/search?q=&page=  proxy the freesound text search (JSON through)
//   POST /fs/get?id=&name=    start the download/decode/install pipeline
//   POST /fs/fetch?url=&name= same pipeline from a direct http(s) MP3 URL
//   GET  /fs/state            pipeline phase/progress for polling
//   POST /fs/query?q=&page=   run the DEVICE-side search (same one the panel runs)
//   GET  /fs/results          the device's parsed results + query store
//
// /fs/search (the raw proxy) and /fs/query (the device search) are different on
// purpose: the proxy hands freesound's JSON straight to a browser that wants to
// parse it itself, while /fs/query drives the machine, so a query typed in the
// browser lands in the SAME result list and recents the panel is showing.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <esp_http_server.h>
#include "esp_log.h"
#include "fs_auth.h"
#include "cJSON.h"
#include "fs_priv.h"

static bool q_param(httpd_req_t *req, const char *key, char *buf, size_t buflen)
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

// httpd_query_key_value does not %-decode; needed for the url= parameter
static void urldecode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') { *o++ = ' '; s++; }
        else *o++ = *s++;
    }
    *o = 0;
}

static esp_err_t send_json_status(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t fs_search_handler(httpd_req_t *req)
{
    char q[96];
    if (!q_param(req, "q", q, sizeof(q)) || !q[0])
        return send_json_status(req, "400 Bad Request", "{\"error\":\"missing q\"}");
    if (!fs_auth_ok())
        return send_json_status(req, "403 Forbidden", "{\"error\":\"no freesound API key set\"}");

    // q arrives still %-encoded from the query string; keep only URL-safe
    // characters so it can be forwarded verbatim
    for (char *p = q; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("%+._-", *p)) *p = '_';

    char page[8] = "1";
    if (q_param(req, "page", page, sizeof(page))) {
        for (char *p = page; *p; p++)
            if (!isdigit((unsigned char)*p)) { strcpy(page, "1"); break; }
        if (!page[0]) strcpy(page, "1");
    }

    strlcpy(fsm.last_query, q, sizeof(fsm.last_query));

    char auth[160];
    fs_auth_query_suffix(auth, sizeof(auth));
    char url[512];
    snprintf(url, sizeof(url),
             "https://freesound.org/apiv2/search/text/?query=%s&page=%s&page_size=16"
             "&fields=id,name,duration,username%s",
             q, page, auth);

    char *buf = NULL;
    int n = fs_http_get(url, &buf, 48 * 1024);
    if (n <= 0) {
        free(buf);
        return send_json_status(req, "502 Bad Gateway", "{\"error\":\"freesound unreachable\"}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, n);
    free(buf);
    return ESP_OK;
}

static esp_err_t fs_state_handler(httpd_req_t *req)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"phase\":\"%s\",\"progress\":%d,\"id\":\"%s\",\"name\":\"%s\","
             "\"err\":\"%s\",\"stack_min\":%u}",
             fs_phase_name(fsm.phase), fsm.progress, fsm.cur_id, fsm.cur_name,
             fsm.err, fsm.stack_min);
    return send_json_status(req, "200 OK", buf);
}

static esp_err_t fs_get_handler(httpd_req_t *req)
{
    char id[16], name[24];
    if (!q_param(req, "id", id, sizeof(id)) || !id[0])
        return send_json_status(req, "400 Bad Request", "{\"error\":\"missing id\"}");
    for (char *p = id; *p; p++)
        if (!isdigit((unsigned char)*p))
            return send_json_status(req, "400 Bad Request", "{\"error\":\"bad id\"}");

    char raw[24];
    if (!q_param(req, "name", raw, sizeof(raw))) raw[0] = 0;
    fs_safe_name(raw, id, name, sizeof(name));

    int r = fs_get_start(id, name);
    if (r == -1)
        return send_json_status(req, "409 Conflict", "{\"error\":\"busy\"}");
    if (r != 0)   // could not start at all — fsm.err says why
        return send_json_status(req, "503 Service Unavailable", "{\"error\":\"cannot start\"}");
    return send_json_status(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t fs_fetch_handler(httpd_req_t *req)
{
    char url[320], name[24];
    if (!q_param(req, "url", url, sizeof(url)) || !url[0])
        return send_json_status(req, "400 Bad Request", "{\"error\":\"missing url\"}");
    urldecode(url);
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return send_json_status(req, "400 Bad Request", "{\"error\":\"http(s) URL required\"}");

    char raw[24];
    if (!q_param(req, "name", raw, sizeof(raw)) || !raw[0])
        return send_json_status(req, "400 Bad Request", "{\"error\":\"missing name\"}");
    fs_safe_name(raw, "", name, sizeof(name));
    if (!name[0])
        return send_json_status(req, "400 Bad Request", "{\"error\":\"bad name\"}");

    int r = fs_fetch_start(url, name);
    if (r == -1)
        return send_json_status(req, "409 Conflict", "{\"error\":\"busy\"}");
    if (r != 0)
        return send_json_status(req, "503 Service Unavailable", "{\"error\":\"cannot start\"}");
    return send_json_status(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t fs_query_handler(httpd_req_t *req)
{
    char q[FS_QUERY_LEN];
    if (!q_param(req, "q", q, sizeof(q)) || !q[0])
        return send_json_status(req, "400 Bad Request", "{\"error\":\"missing q\"}");
    urldecode(q);
    if (!fs_auth_ok())
        return send_json_status(req, "403 Forbidden", "{\"error\":\"no freesound API key set\"}");

    char ps[8];
    int page = q_param(req, "page", ps, sizeof(ps)) ? atoi(ps) : 1;
    if (page < 1) page = 1;

    int r = fs_search_start(q, page);
    if (r == -1) return send_json_status(req, "409 Conflict", "{\"error\":\"busy\"}");
    if (r != 0)  return send_json_status(req, "503 Service Unavailable", "{\"error\":\"cannot start\"}");
    return send_json_status(req, "200 OK", "{\"ok\":true}");
}

// The device's own view: what the panel is showing, so the web card renders the
// same list instead of a second one that can disagree with it.
static esp_err_t fs_results_handler(httpd_req_t *req)
{
    static const char *const ST[] = { "idle", "searching", "ok", "error" };
    cJSON *o = cJSON_CreateObject();
    int st = fsm.search_state;
    if (st < 0 || st > 3) st = 0;
    cJSON_AddStringToObject(o, "state", ST[st]);
    cJSON_AddStringToObject(o, "query", fsm.last_query);
    cJSON_AddNumberToObject(o, "page", fsm.page);
    cJSON_AddNumberToObject(o, "total", fsm.total);
    cJSON_AddNumberToObject(o, "per_page", FS_RESULTS_MAX);
    if (fsm.serr[0]) cJSON_AddStringToObject(o, "err", fsm.serr);
    // free-stack low-water mark for the search task, in bytes: the instrument
    // that turns "it panicked" into a number (see fs_machine.c)
    cJSON_AddNumberToObject(o, "stack_min", (double)fsm.search_stack_min);

    cJSON *arr = cJSON_AddArrayToObject(o, "results");
    int n = fsm.n_results;
    if (n > FS_RESULTS_MAX) n = FS_RESULTS_MAX;
    for (int i = 0; i < n; i++) {
        const fs_result_t *r = &fsm.results[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", r->id);
        cJSON_AddStringToObject(e, "name", r->name);
        cJSON_AddStringToObject(e, "user", r->user);
        cJSON_AddNumberToObject(e, "dur", r->dur_ds / 10.0);
        cJSON_AddItemToArray(arr, e);
    }

    cJSON *rc = cJSON_AddArrayToObject(o, "recents");
    for (int i = 0; i < fsm.n_recents; i++) cJSON_AddItemToArray(rc, cJSON_CreateString(fsm.recents[i]));
    cJSON *sv = cJSON_AddArrayToObject(o, "saved");
    for (int i = 0; i < fsm.n_saved; i++) cJSON_AddItemToArray(sv, cJSON_CreateString(fsm.saved[i]));

    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    esp_err_t rc2 = send_json_status(req, "200 OK", js ? js : "{}");
    free(js);
    return rc2;
}

const httpd_uri_t fs_web_uris[] = {
    { .uri = "/fs/search", .method = HTTP_GET,  .handler = fs_search_handler },
    { .uri = "/fs/state",  .method = HTTP_GET,  .handler = fs_state_handler },
    { .uri = "/fs/get",    .method = HTTP_POST, .handler = fs_get_handler },
    { .uri = "/fs/fetch",  .method = HTTP_POST, .handler = fs_fetch_handler },
    { .uri = "/fs/query",  .method = HTTP_POST, .handler = fs_query_handler },
    { .uri = "/fs/results",.method = HTTP_GET,  .handler = fs_results_handler },
};
_Static_assert(sizeof(fs_web_uris) / sizeof(fs_web_uris[0]) == FS_WEB_URIS_N,
               "fs_web_uris[] and FS_WEB_URIS_N disagree — machine_ui_t.n_web_uris "
               "is a hand-written count and a stale one silently drops endpoints");
const int fs_web_n_uris = sizeof(fs_web_uris) / sizeof(fs_web_uris[0]);
