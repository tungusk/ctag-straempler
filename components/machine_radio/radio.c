// Internet radio machine (see radio_priv.h). HTTP reader + helix decode on one
// unpinned task -> PSRAM stereo ring -> process() drains it. Control via the web
// (/radio/*) and the on-device station picker.
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "machine.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#define MIPS                 // helix platform selector (the project's generic-C path; see util/mp3.c)
#include "mp3dec.h"
#include "radio_priv.h"

static const char *TAG = "RADIO";

radio_state_t rd;

// A few known-good icecast MP3 mounts (44.1 kHz) so the machine is usable
// without typing a URL. Plain HTTP — no TLS handshake cost.
const radio_station_t radio_stations[] = {
    { "Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "Drone Zone",   "http://ice1.somafm.com/dronezone-128-mp3" },
    { "DEF CON",      "http://ice1.somafm.com/defcon-128-mp3" },
    { "Lush",         "http://ice1.somafm.com/lush-128-mp3" },
    { "Indie Pop",    "http://ice1.somafm.com/indiepop-128-mp3" },
};
const int radio_n_stations = sizeof(radio_stations) / sizeof(radio_stations[0]);

static volatile bool s_stream_run = false;   // stream task is alive
static volatile bool s_stop = false;         // request the stream task to exit

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

static void stream_task(void *pv)
{
    s_stream_run = true;
    HMP3Decoder dec = NULL;
    uint8_t *inbuf = NULL;
    short *pcm = NULL;
    esp_http_client_handle_t cl = NULL;

    esp_http_client_config_t cfg = {
        .url = rd.url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,   // harmless on http, required on https
    };
    cl = esp_http_client_init(&cfg);
    if (!cl) { set_err("http init failed"); goto done; }
    if (esp_http_client_open(cl, 0) != ESP_OK) { set_err("connect failed"); goto done; }
    esp_http_client_fetch_headers(cl);   // icecast: unknown length -> returns 0, fine

    dec   = MP3InitDecoder();
    inbuf = malloc(RADIO_IN_SIZE);
    pcm   = malloc(2 * 1152 * sizeof(short));
    if (!dec || !inbuf || !pcm) { set_err("decoder OOM"); goto done; }

    int fill = 0;
    bool first = true;
    while (!s_stop) {
        // top up the byte buffer from the socket (blocks -> paces to stream rate)
        if (fill < RADIO_IN_SIZE) {
            int r = esp_http_client_read(cl, (char *)inbuf + fill, RADIO_IN_SIZE - fill);
            if (r < 0) { set_err("stream read error"); break; }
            if (r == 0) { set_err("stream ended"); break; }   // icecast dropped us
            fill += r;
        }
        // decode every complete frame currently in the buffer
        int cur = 0;   // bytes consumed from the front so far
        while (fill - cur > RADIO_MIN_FRAME && !s_stop) {
            int off = MP3FindSyncWord(inbuf + cur, fill - cur);
            if (off < 0) { cur = fill - 1; break; }   // no sync; keep last byte (partial)
            cur += off;
            unsigned char *rp = inbuf + cur;
            int bl = fill - cur;
            int err = MP3Decode(dec, &rp, &bl, pcm, 0);
            if (err == ERR_MP3_INDATA_UNDERFLOW) break;   // need more bytes -> refill
            int used = (fill - cur) - bl;
            if (err == 0) {
                MP3FrameInfo fi;
                MP3GetLastFrameInfo(dec, &fi);
                if (first && fi.nChans > 0) {
                    first = false;
                    rd.bitrate = fi.bitrate / 1000;
                    rd.samprate = fi.samprate;
                    rd.nchans = fi.nChans;
                    if (fi.samprate != RADIO_RATE) { set_err("unsupported rate (v1: 44.1k)"); break; }
                    ESP_LOGI(TAG, "stream: %d kbps, %d Hz, %d ch", rd.bitrate, fi.samprate, fi.nChans);
                }
                if (fi.outputSamps > 0) ring_write(pcm, fi.outputSamps, fi.nChans);
                cur += used;
            } else {
                cur += (used > 0 ? used : 1);   // bad frame: guarantee forward progress
            }
        }
        // slide the unconsumed tail to the front
        if (cur > 0 && cur <= fill) { memmove(inbuf, inbuf + cur, fill - cur); fill -= cur; }
        if (fill >= RADIO_IN_SIZE) fill = 0;   // full of garbage, no sync -> drop & resync
    }

done:
    if (dec) MP3FreeDecoder(dec);
    free(inbuf);
    free(pcm);
    if (cl) { esp_http_client_close(cl); esp_http_client_cleanup(cl); }
    if (rd.state != RADIO_ERROR) rd.state = RADIO_STOPPED;
    s_stream_run = false;
    vTaskDelete(NULL);
}

void radio_stop_stream(void)
{
    s_stop = true;
    for (int i = 0; i < 250 && s_stream_run; i++) vTaskDelay(pdMS_TO_TICKS(10));   // up to 2.5 s
    if (rd.state != RADIO_ERROR) rd.state = RADIO_STOPPED;
    rd.wpos = rd.rpos = 0;
}

void radio_play_url(const char *url, const char *name)
{
    if (!url || !url[0]) return;
    radio_stop_stream();               // kill any current stream first
    strlcpy(rd.url, url, sizeof(rd.url));
    strlcpy(rd.station, name ? name : "custom", sizeof(rd.station));
    rd.wpos = rd.rpos = 0;
    rd.underruns = 0;
    rd.bitrate = rd.samprate = rd.nchans = 0;
    rd.err[0] = 0;
    rd.state = RADIO_BUFFERING;
    s_stop = false;
    if (!rd.ring) { set_err("no ring (machine stopped)"); return; }
    if (xTaskCreate(stream_task, "radio_dl", 20480, NULL, 5, NULL) != pdPASS)
        set_err("stream task create failed");
}

void radio_play_station(int idx)
{
    if (idx < 0 || idx >= radio_n_stations) return;
    rd.sel = idx;
    radio_play_url(radio_stations[idx].url, radio_stations[idx].name);
}

// ---- machine lifecycle ------------------------------------------------------
static esp_err_t radio_start(void)
{
    memset(&rd, 0, sizeof(rd));
    rd.ring = heap_caps_malloc((size_t)RADIO_RING_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!rd.ring) { ESP_LOGE(TAG, "PSRAM ring alloc failed"); return ESP_ERR_NO_MEM; }
    rd.state = RADIO_STOPPED;
    s_stop = false;
    s_stream_run = false;
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
        if (cJSON_IsNumber(j) && j->valueint >= 0 && j->valueint < radio_n_stations)
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
