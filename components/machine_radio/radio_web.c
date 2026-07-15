// Radio REST endpoints — served only while the machine is active (web_uris):
//   POST /radio/play?station=N   play a built-in station
//   POST /radio/play?url=<enc>   play a custom stream URL (url-encoded)
//   POST /radio/stop             stop
//   GET  /radio/state            JSON status + the built-in station list
#include <string.h>
#include <stdlib.h>
#include <esp_http_server.h>
#include "cJSON.h"
#include "radio_priv.h"

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

// in-place url-decode (%XX + '+'); httpd_query_key_value does not decode
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = p[1], lo = p[2];
            hi = (hi >= 'a') ? hi - 'a' + 10 : (hi >= 'A') ? hi - 'A' + 10 : hi - '0';
            lo = (lo >= 'a') ? lo - 'a' + 10 : (lo >= 'A') ? lo - 'A' + 10 : lo - '0';
            *o++ = (char)((hi << 4) | lo);
            p += 2;
        } else if (*p == '+') *o++ = ' ';
        else *o++ = *p;
    }
    *o = 0;
}

static esp_err_t send_json(httpd_req_t *req, const char *s)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, s);
}

static esp_err_t radio_play_handler(httpd_req_t *req)
{
    char buf[RADIO_URL_LEN];
    if (qparam(req, "station", buf, sizeof(buf))) {
        radio_play_station(atoi(buf));
        return send_json(req, "{\"ok\":true}");
    }
    if (qparam(req, "url", buf, sizeof(buf))) {
        url_decode(buf);
        if (strncmp(buf, "http", 4) != 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url must be http(s)://");
            return ESP_FAIL;
        }
        radio_play_url(buf, "custom");
        return send_json(req, "{\"ok\":true}");
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need ?station=N or ?url=");
    return ESP_FAIL;
}

static esp_err_t radio_stop_handler(httpd_req_t *req)
{
    radio_stop_stream();
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t radio_state_handler(httpd_req_t *req)
{
    static const char *const names[] = { "stopped", "buffering", "playing", "error" };
    cJSON *o = cJSON_CreateObject();
    int st = rd.state; if (st < 0 || st > 3) st = 0;
    cJSON_AddStringToObject(o, "state", names[st]);
    cJSON_AddStringToObject(o, "station", rd.station);
    cJSON_AddStringToObject(o, "title", rd.title);
    cJSON_AddNumberToObject(o, "reconnects", (double)rd.reconnects);
    cJSON_AddStringToObject(o, "url", rd.url);
    cJSON_AddNumberToObject(o, "bitrate", rd.bitrate);
    cJSON_AddNumberToObject(o, "samprate", rd.samprate);
    cJSON_AddNumberToObject(o, "nchans", rd.nchans);
    cJSON_AddNumberToObject(o, "underruns", (double)rd.underruns);
    // ring fill %, so the UI can show buffering health
    uint32_t avail = rd.wpos - rd.rpos;
    int pct = (int)((uint64_t)avail * 100 / RADIO_RING_FRAMES);
    cJSON_AddNumberToObject(o, "buf", pct > 100 ? 100 : pct);
    cJSON_AddNumberToObject(o, "sel", rd.sel);
    if (rd.err[0]) cJSON_AddStringToObject(o, "err", rd.err);
    cJSON *arr = cJSON_AddArrayToObject(o, "stations");
    for (int i = 0; i < radio_n_stations; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(radio_stations[i].name));
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    esp_err_t rc = send_json(req, s ? s : "{}");
    free(s);
    return rc;
}

const httpd_uri_t radio_web_uris[] = {
    { .uri = "/radio/play",  .method = HTTP_POST, .handler = radio_play_handler },
    { .uri = "/radio/stop",  .method = HTTP_POST, .handler = radio_stop_handler },
    { .uri = "/radio/state", .method = HTTP_GET,  .handler = radio_state_handler },
};
const int radio_web_n_uris = sizeof(radio_web_uris) / sizeof(radio_web_uris[0]);
