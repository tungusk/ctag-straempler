// Internet radio machine (see radio_priv.h). HTTP reader + helix decode on one
// unpinned task -> PSRAM stereo ring -> process() drains it. Control via the web
// (/radio/*) and the on-device station picker.
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "fileio.h"
#include "machine.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#define MIPS                 // helix platform selector (the project's generic-C path; see util/mp3.c)
#include "mp3dec.h"
#include "radio_priv.h"

static const char *TAG = "RADIO";

radio_state_t rd;

// Built-in icecast MP3 mounts (44.1 kHz) so the machine is usable without
// typing a URL. Plain HTTP — no TLS handshake cost. Saved favourites append
// after these (loaded from usr/radio.jsn).
static const radio_station_t DEFAULTS[RADIO_N_DEFAULT] = {
    { "Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "Drone Zone",   "http://ice1.somafm.com/dronezone-128-mp3" },
    { "DEF CON",      "http://ice1.somafm.com/defcon-128-mp3" },
    { "Lush",         "http://ice1.somafm.com/lush-128-mp3" },
    { "Indie Pop",    "http://ice1.somafm.com/indiepop-128-mp3" },
    { "SPAZ",         "http://radio.spaz.org:8050/radio" },
};
radio_station_t rd_stations[RADIO_MAX_ST];
int rd_n_stations = 0;

#define RADIO_STATIONS_FILE "/sdcard/usr/radio.jsn"

static void radio_stations_save(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = RADIO_N_DEFAULT; i < rd_n_stations; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", rd_stations[i].name);
        cJSON_AddStringToObject(e, "url", rd_stations[i].url);
        cJSON_AddItemToArray(arr, e);
    }
    char *s = cJSON_Print(arr);
    cJSON_Delete(arr);
    if (s) { writeJSONFile(RADIO_STATIONS_FILE, s); free(s); }
}

void radio_stations_load(void)
{
    memcpy(rd_stations, DEFAULTS, sizeof(DEFAULTS));
    rd_n_stations = RADIO_N_DEFAULT;
    cJSON *root = readJSONFileAsCJSON(RADIO_STATIONS_FILE);
    if (root && cJSON_IsArray(root)) {
        cJSON *e;
        cJSON_ArrayForEach(e, root) {
            if (rd_n_stations >= RADIO_MAX_ST) break;
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(e, "name");
            cJSON *u  = cJSON_GetObjectItemCaseSensitive(e, "url");
            if (cJSON_IsString(u) && u->valuestring[0]) {
                strlcpy(rd_stations[rd_n_stations].url, u->valuestring, RADIO_URL_LEN);
                strlcpy(rd_stations[rd_n_stations].name,
                        (cJSON_IsString(nm) && nm->valuestring[0]) ? nm->valuestring : "custom", RADIO_NAME_LEN);
                rd_n_stations++;
            }
        }
    }
    if (root) cJSON_Delete(root);
}

int radio_station_add(const char *name, const char *url)
{
    if (!url || strncmp(url, "http", 4) != 0) return -1;
    if (rd_n_stations >= RADIO_MAX_ST) return -1;
    strlcpy(rd_stations[rd_n_stations].url, url, RADIO_URL_LEN);
    strlcpy(rd_stations[rd_n_stations].name, (name && name[0]) ? name : "custom", RADIO_NAME_LEN);
    rd_n_stations++;
    radio_stations_save();
    return 0;
}

int radio_station_del(int idx)
{
    if (idx < RADIO_N_DEFAULT || idx >= rd_n_stations) return -1;   // built-ins are permanent
    for (int i = idx; i < rd_n_stations - 1; i++) rd_stations[i] = rd_stations[i + 1];
    rd_n_stations--;
    radio_stations_save();
    return 0;
}

static volatile int  s_ntasks = 0;           // # live stream tasks (0/1 normally; briefly >1 on a fast restart)
static volatile bool s_stop = false;         // request the stream task(s) to exit
static volatile int  s_gen = 0;              // play generation: a task whose gen != s_gen retires WITHOUT
                                             // touching the ring or rd.state (prevents the two-task race that
                                             // wedged radio when a station change raced a blocked socket read)

static void set_err(const char *m)
{
    strlcpy(rd.err, m, sizeof(rd.err));
    rd.state = RADIO_ERROR;
    ESP_LOGW(TAG, "%s", m);
}

// write one decoded frame's PCM (interleaved shorts, `outputSamps` total across
// `nch` channels) into the ring, expanding mono -> stereo. Backpressure: block
// if the ring is full (a socket burst outran playback) so the decoder paces to
// real time. Promotes BUFFERING -> PLAYING once the pre-buffer is met.
static void ring_write(const short *pcm, int outputSamps, int nch)
{
    int frames = (nch > 0) ? outputSamps / nch : 0;
    for (int i = 0; i < frames; i++) {
        while (!s_stop && (rd.wpos - rd.rpos) >= RADIO_RING_FRAMES - 2)
            vTaskDelay(1);
        if (s_stop) return;
        int16_t l, r;
        if (nch == 2) { l = pcm[2 * i]; r = pcm[2 * i + 1]; }
        else          { l = r = pcm[i]; }
        uint32_t idx = (rd.wpos % RADIO_RING_FRAMES) * 2;
        rd.ring[idx] = l;
        rd.ring[idx + 1] = r;
        rd.wpos++;
    }
    if (rd.state == RADIO_BUFFERING && (rd.wpos - rd.rpos) >= RADIO_LOW_WATER)
        rd.state = RADIO_PLAYING;
}

// ---- ICY metadata (now-playing) --------------------------------------------
static int s_audio_left;              // audio bytes until the next metadata block
static volatile int s_meta_int_hdr;   // icy-metaint from the RESPONSE header callback

// esp_http_client_get_header reads REQUEST headers, not response ones — the only
// way to read the server's icy-metaint is this event callback during open().
static esp_err_t radio_http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key &&
        strcasecmp(e->header_key, "icy-metaint") == 0 && e->header_value)
        s_meta_int_hdr = atoi(e->header_value);
    return ESP_OK;
}

static void radio_apply_meta(const char *m)
{
    const char *p = strstr(m, "StreamTitle=");
    if (!p) return;
    p += 12;
    if (*p == '\'') p++;
    const char *e = strstr(p, "';");
    if (!e) e = strchr(p, '\'');
    int n = e ? (int)(e - p) : (int)strlen(p);
    if (n < 0) n = 0;
    if (n > RADIO_TITLE_LEN - 1) n = RADIO_TITLE_LEN - 1;
    memcpy(rd.title, p, n);
    rd.title[n] = 0;
}

// read up to `want` AUDIO bytes, transparently consuming ICY metadata blocks
// (every meta_int bytes the stream inserts: 1 length byte L, then L*16 bytes)
static int icy_read(esp_http_client_handle_t cl, uint8_t *dst, int want, int meta_int)
{
    if (meta_int <= 0) return esp_http_client_read(cl, (char *)dst, want);
    int got = 0;
    while (got < want) {
        if (s_audio_left <= 0) {
            uint8_t lb;
            int r = esp_http_client_read(cl, (char *)&lb, 1);
            if (r <= 0) return got > 0 ? got : r;
            int mlen = (int)lb * 16;
            if (mlen > 0) {
                char mb[512];
                int mr = 0;
                while (mr < mlen) {
                    char *into = (mr < (int)sizeof(mb) - 1) ? mb + mr : NULL;
                    char waste[64];
                    int cap = mlen - mr;
                    if (into) { if (cap > (int)sizeof(mb) - 1 - mr) cap = (int)sizeof(mb) - 1 - mr; }
                    else      { if (cap > (int)sizeof(waste)) cap = (int)sizeof(waste); }
                    int r2 = esp_http_client_read(cl, into ? into : waste, cap);
                    if (r2 <= 0) return got > 0 ? got : r2;
                    mr += r2;
                }
                mb[mr < (int)sizeof(mb) ? mr : (int)sizeof(mb) - 1] = 0;
                radio_apply_meta(mb);
            }
            s_audio_left = meta_int;
        }
        int chunk = want - got;
        if (chunk > s_audio_left) chunk = s_audio_left;
        int r = esp_http_client_read(cl, (char *)dst + got, chunk);
        if (r <= 0) return got > 0 ? got : r;
        got += r;
        s_audio_left -= r;
    }
    return got;
}

static void stream_task(void *pv)
{
    int gen = (int)(intptr_t)pv;   // this task's generation; retire when superseded
    s_ntasks++;
    HMP3Decoder dec = MP3InitDecoder();
    uint8_t *inbuf = malloc(RADIO_IN_SIZE);
    short *pcm = malloc(2 * 1152 * sizeof(short));
    if (!dec || !inbuf || !pcm) { set_err("decoder OOM"); goto done; }

    int attempts = 0;
    while (!s_stop && gen == s_gen) {
        esp_http_client_config_t cfg = {
            // shorter timeout so a superseded/dead connection unblocks and the
            // task exits promptly (was 15s -> a station change could orphan it)
            .url = rd.url, .method = HTTP_METHOD_GET, .timeout_ms = 6000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = radio_http_evt,     // captures icy-metaint from the response
        };
        esp_http_client_handle_t cl = esp_http_client_init(&cfg);
        if (!cl) { set_err("http init failed"); break; }
        esp_http_client_set_header(cl, "Icy-MetaData", "1");   // ask for now-playing
        s_meta_int_hdr = 0;
        if (esp_http_client_open(cl, 0) != ESP_OK) {
            esp_http_client_cleanup(cl);
            if (++attempts > 6) { set_err("connect failed"); break; }
            for (int i = 0; i < 100 && !s_stop && gen == s_gen; i++) vTaskDelay(pdMS_TO_TICKS(10));   // ~1s backoff
            continue;
        }
        esp_http_client_fetch_headers(cl);
        int meta_int = s_meta_int_hdr;   // set by radio_http_evt during open/fetch
        s_audio_left = meta_int;

        int fill = 0;
        bool disconnected = false;
        while (!s_stop && !disconnected && gen == s_gen) {
            if (fill < RADIO_IN_SIZE) {
                int r = icy_read(cl, inbuf + fill, RADIO_IN_SIZE - fill, meta_int);
                if (r <= 0) { disconnected = true; break; }   // dropped -> reconnect
                fill += r;
                attempts = 0;                                  // real data: reset the backoff
            }
            int cur = 0;
            while (fill - cur > RADIO_MIN_FRAME && !s_stop && gen == s_gen) {
                int off = MP3FindSyncWord(inbuf + cur, fill - cur);
                if (off < 0) { cur = fill - 1; break; }
                cur += off;
                unsigned char *rp = inbuf + cur;
                int bl = fill - cur;
                int err = MP3Decode(dec, &rp, &bl, pcm, 0);
                if (err == ERR_MP3_INDATA_UNDERFLOW) break;
                int used = (fill - cur) - bl;
                if (err == 0) {
                    MP3FrameInfo fi;
                    MP3GetLastFrameInfo(dec, &fi);
                    if (rd.samprate == 0 && fi.nChans > 0) {
                        rd.bitrate = fi.bitrate / 1000;
                        rd.samprate = fi.samprate;
                        rd.nchans = fi.nChans;
                        if (fi.samprate != RADIO_RATE) { set_err("unsupported rate (v1: 44.1k)"); s_stop = true; break; }
                        ESP_LOGI(TAG, "stream: %d kbps, %d Hz, %d ch, meta_int %d", rd.bitrate, fi.samprate, fi.nChans, meta_int);
                    }
                    if (fi.outputSamps > 0 && gen == s_gen) ring_write(pcm, fi.outputSamps, fi.nChans);
                    cur += used;
                } else {
                    cur += (used > 0 ? used : 1);
                }
            }
            if (cur > 0 && cur <= fill) { memmove(inbuf, inbuf + cur, fill - cur); fill -= cur; }
            if (fill >= RADIO_IN_SIZE) fill = 0;
        }
        esp_http_client_close(cl);
        esp_http_client_cleanup(cl);
        if (s_stop || gen != s_gen) break;
        // the stream dropped: drain to silence + re-buffer on reconnect, with a
        // short backoff. Give up only after several CONSECUTIVE failures.
        rd.reconnects++;
        if (rd.state == RADIO_PLAYING) rd.state = RADIO_BUFFERING;
        if (++attempts > 8) { set_err("stream lost"); break; }
        for (int i = 0; i < 80 && !s_stop && gen == s_gen; i++) vTaskDelay(pdMS_TO_TICKS(10));   // ~0.8s
    }

done:
    if (dec) MP3FreeDecoder(dec);
    free(inbuf);
    free(pcm);
    // don't stomp a newer task's state — only the current generation owns rd.state
    if (gen == s_gen && rd.state != RADIO_ERROR) rd.state = RADIO_STOPPED;
    s_ntasks--;
    vTaskDelete(NULL);
}

// FULL stop (machine switch / ring free): must guarantee no task is still
// writing the ring before the caller frees it, so this one waits.
void radio_stop_stream(void)
{
    s_stop = true;
    s_gen++;                           // retire every running task
    for (int i = 0; i < 700 && s_ntasks > 0; i++) vTaskDelay(pdMS_TO_TICKS(10));   // up to 7 s (> read timeout)
    if (rd.state != RADIO_ERROR) rd.state = RADIO_STOPPED;
    rd.wpos = rd.rpos = 0;
}

void radio_play_url(const char *url, const char *name)
{
    if (!url || !url[0]) return;
    if (!rd.ring) { set_err("no ring (machine stopped)"); return; }
    // Bump the generation to retire any current task: it will stop writing the
    // ring / touching rd.state immediately (gen guard) and exit on its own when
    // its socket unblocks. So we do NOT block here — a new play is instant and
    // can never orphan a second live task fighting over the ring.
    s_gen++;
    s_stop = false;
    // give a fast-exiting old task a brief moment to drain (common case: <100ms),
    // purely to avoid piling up tasks; correctness does not depend on it
    for (int i = 0; i < 40 && s_ntasks > 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
    strlcpy(rd.url, url, sizeof(rd.url));
    strlcpy(rd.station, name ? name : "custom", sizeof(rd.station));
    rd.wpos = rd.rpos = 0;
    rd.underruns = 0;
    rd.reconnects = 0;
    rd.bitrate = rd.samprate = rd.nchans = 0;
    rd.title[0] = 0;
    rd.err[0] = 0;
    rd.state = RADIO_BUFFERING;
    int gen = ++s_gen;                 // this play's generation (bump again after the drain wait)
    if (xTaskCreate(stream_task, "radio_dl", 20480, (void *)(intptr_t)gen, 5, NULL) != pdPASS)
        set_err("stream task create failed");
}

void radio_play_station(int idx)
{
    if (idx < 0 || idx >= rd_n_stations) return;
    rd.sel = idx;
    radio_play_url(rd_stations[idx].url, rd_stations[idx].name);
}

// ---- machine lifecycle ------------------------------------------------------
static esp_err_t radio_start(void)
{
    memset(&rd, 0, sizeof(rd));
    radio_stations_load();       // defaults + SD-saved favourites
    rd.ring = heap_caps_malloc((size_t)RADIO_RING_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!rd.ring) { ESP_LOGE(TAG, "PSRAM ring alloc failed"); return ESP_ERR_NO_MEM; }
    rd.state = RADIO_STOPPED;
    s_stop = false;
    s_ntasks = 0;
    return ESP_OK;
}

static void radio_stop(void)
{
    radio_stop_stream();
    free(rd.ring);
    rd.ring = NULL;
}

static void radio_process(int32_t out[MACHINE_BLOCK],
                          const int32_t in[MACHINE_BLOCK],
                          const machine_io_t *io)
{
    (void)in; (void)io;
    int frames = MACHINE_BLOCK / 2;
    if (rd.state != RADIO_PLAYING || !rd.ring) {
        memset(out, 0, MACHINE_BLOCK * sizeof(int32_t));   // silence unless playing
        return;
    }
    for (int f = 0; f < frames; f++) {
        if (rd.rpos < rd.wpos) {
            uint32_t idx = (rd.rpos % RADIO_RING_FRAMES) * 2;
            out[f * 2]     = ((int32_t)rd.ring[idx]) << 16;
            out[f * 2 + 1] = ((int32_t)rd.ring[idx + 1]) << 16;
            rd.rpos++;
        } else {
            out[f * 2] = out[f * 2 + 1] = 0;   // underrun: silence + re-buffer
            rd.underruns++;
            rd.state = RADIO_BUFFERING;
        }
    }
}

static cJSON *radio_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "sel", rd.sel);
    return o;
}

static void radio_preset_load(const cJSON *node)
{
    rd.sel = 0;
    if (node) {
        cJSON *j = cJSON_GetObjectItemCaseSensitive(node, "sel");
        if (cJSON_IsNumber(j) && j->valueint >= 0 && j->valueint < RADIO_MAX_ST)
            rd.sel = j->valueint;
    }
}

extern const machine_ui_t radio_menu_ui;

const machine_t machine_radio = {
    .name = "Radio",
    .start = radio_start,
    .stop = radio_stop,
    .process = radio_process,
    .preset_save = radio_preset_save,
    .preset_load = radio_preset_load,
    .ui = &radio_menu_ui,
};
