// Freesound machine web endpoints — registered by the core httpd while this
// machine is active (machine_ui_t.web_uris):
//   GET  /fs/search?q=&page=  proxy the freesound text search (JSON through)
//   POST /fs/get?id=&name=    start the download/decode/install pipeline
//   POST /fs/fetch?url=&name= same pipeline from a direct http(s) MP3 URL
//   GET  /fs/state            pipeline phase/progress for polling
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <esp_http_server.h>
#include "esp_log.h"
#include "fs_auth.h"
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
             "{\"phase\":\"%s\",\"progress\":%d,\"id\":\"%s\",\"name\":\"%s\",\"err\":\"%s\"}",
             fs_phase_name(fsm.phase), fsm.progress, fsm.cur_id, fsm.cur_name, fsm.err);
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

    if (!q_param(req, "name", name, sizeof(name)) || !name[0])
        snprintf(name, sizeof(name), "FS%s", id);
    // library-safe name: alnum/_/-, max 12 chars
    int w = 0;
    for (int i = 0; name[i] && w < 12; i++)
        if (isalnum((unsigned char)name[i]) || name[i] == '_' || name[i] == '-')
            name[w++] = name[i];
    name[w] = 0;
    if (!name[0]) snprintf(name, sizeof(name), "FS%s", id);

    if (fs_get_start(id, name) != 0)
        return send_json_status(req, "409 Conflict", "{\"error\":\"busy\"}");
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

    if (!q_param(req, "name", name, sizeof(name)) || !name[0])
        return send_json_status(req, "400 Bad Request", "{\"error\":\"missing name\"}");
    // library-safe name: alnum/_/-, max 12 chars
    int w = 0;
    for (int i = 0; name[i] && w < 12; i++)
        if (isalnum((unsigned char)name[i]) || name[i] == '_' || name[i] == '-')
            name[w++] = name[i];
    name[w] = 0;
    if (!name[0])
        return send_json_status(req, "400 Bad Request", "{\"error\":\"bad name\"}");

    if (fs_fetch_start(url, name) != 0)
        return send_json_status(req, "409 Conflict", "{\"error\":\"busy\"}");
    return send_json_status(req, "200 OK", "{\"ok\":true}");
}

const httpd_uri_t fs_web_uris[] = {
    { .uri = "/fs/search", .method = HTTP_GET,  .handler = fs_search_handler },
    { .uri = "/fs/state",  .method = HTTP_GET,  .handler = fs_state_handler },
    { .uri = "/fs/get",    .method = HTTP_POST, .handler = fs_get_handler },
    { .uri = "/fs/fetch",  .method = HTTP_POST, .handler = fs_fetch_handler },
};
const int fs_web_n_uris = sizeof(fs_web_uris) / sizeof(fs_web_uris[0]);
