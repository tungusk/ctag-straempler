// core audio transport — machine-agnostic. CV in, I2S in/out, recording tap,
// status snapshot; the active machine does everything in between.
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "layer3.h"                      // shine MP3 encoder (broadcast /live.mp3)
#include "wifi.h"
#include "audio.h"
#include "recording.h"
#include "machine.h"
#include "beatlisten.h"
#include "spi_per.h"
#include "i2s_per.h"
#include "pin_defs.h"

static xQueueHandle control_queue = NULL;
static TaskHandle_t audio_task_h;
static audio_status_t _audio_status;
static portMUX_TYPE _status_mux = portMUX_INITIALIZER_UNLOCKED;

void audio_get_status(audio_status_t *out) {
    portENTER_CRITICAL(&_status_mux);
    *out = _audio_status;
    portEXIT_CRITICAL(&_status_mux);
}

void audio_get_cv(uint16_t out[8]) {
    portENTER_CRITICAL(&_status_mux);
    memcpy(out, _audio_status.cv, sizeof(_audio_status.cv));
    portEXIT_CRITICAL(&_status_mux);
}

void audio_status_set_voices(const char *v0, const char *v1) {
    portENTER_CRITICAL(&_status_mux);
    strncpy(_audio_status.v0, v0 ? v0 : "", 31); _audio_status.v0[31] = 0;
    strncpy(_audio_status.v1, v1 ? v1 : "", 63); _audio_status.v1[63] = 0;
    portEXIT_CRITICAL(&_status_mux);
}

// teleremote soft triggers: each entry holds the tick until which that gate
// is held asserted; the audio task ANDs them into the hardware reading
static volatile TickType_t s_remote_trig_until[2] = {0, 0};

void audio_remote_trig(int t, int ms) {
    if (t < 0 || t > 1) return;
    s_remote_trig_until[t] = xTaskGetTickCount() + pdMS_TO_TICKS(ms > 0 ? ms : 30);
}

// teleremote CV overrides — the mirror of the soft trigs: while fresh, the
// override substitutes for the ADC reading, so a web-driven knob is
// indistinguishable from a physical one; on timeout the physical knob is
// back in charge (a stale web value can never pin a channel)
static volatile TickType_t s_remote_cv_until[8] = {0};
static volatile uint16_t   s_remote_cv_val[8] = {0};

void audio_remote_cv(int ch, int v, int ms) {
    if (ch < 0 || ch > 7) return;
    if (v < 0) v = 0;
    if (v > 4095) v = 4095;
    s_remote_cv_val[ch] = (uint16_t)v;
    s_remote_cv_until[ch] = xTaskGetTickCount() + pdMS_TO_TICKS(ms > 0 ? ms : 250);
}

// ---- output bounce: record the machine's OUTPUT bus to a pool take ----------
// Core service — works for ANY active machine (radio/synth/deck blend). While
// active, the audio task feeds the recorder `out` (post-process, pre clock-out)
// instead of the line input, so "sample the radio" etc. lands a REC_ take.
static volatile bool s_bounce = false;
void audio_bounce_start(void) {
    if (s_bounce || recording_is_active()) return;
    recording_set_enabled(true);
    recording_set_prefix("BNC");                     // bounces are BNC_, not REC_
    s_bounce = true;
    recording_start(-1);                             // vid -1 = no auto-load into a voice
    if (!recording_is_active()) { s_bounce = false; recording_set_prefix("REC"); }
}
void audio_bounce_stop(void) {
    if (!s_bounce) return;
    recording_stop();
    s_bounce = false;
    recording_set_prefix("REC");                     // restore for sampler takes
}
bool audio_bounce_active(void) { return s_bounce; }

// ---- broadcast: stream the OUTPUT bus live over a dedicated socket ----------
// A raw TCP server on BC_PORT (NOT the shared httpd — a forever-streaming handler
// there would freeze the whole web UI). One client: point a browser / VLC at
// http://<ip>:8000/ (endless stereo WAV, LAN) or http://<ip>:8000/live.mp3
// (Shine 96 kbps MONO, icecast-style — #17, internet-friendly at 12 KB/s). The
// audio task pushes int16 stereo output into a PSRAM ring; the server drains it,
// dropping the OLDEST audio if the client lags (a live stream stays current).
// The MP3 encoder exists only while a .mp3 client is connected (CPU is paid
// per-listen, not always-on); shine's ~100 KB state lives in PSRAM. Measured
// 2026-07-16 (see /bcast/state enc_us, budget 26.1 ms/pass): idle/synth ~18 ms,
// deck playing ~13 ms = realtime; RADIO playing ~39 ms = NOT realtime (helix
// and shine thrash the PSRAM cache) — re-broadcasting Radio drops audio, use
// the LAN WAV stream or bounce for that.
#define BC_PORT   8000
#define BC_FRAMES 44100                    // ~1 s stereo ring
static int16_t *s_bc_ring = NULL;
static volatile uint32_t s_bc_w = 0, s_bc_r = 0;
static volatile bool s_bc_on = false;      // a client is connected
static const char *s_bc_err = "ok";        // last MP3-path failure, for /bcast/state
static volatile uint32_t s_bc_enc_us = 0;  // smoothed encode cost per 26.1ms pass

bool audio_broadcast_active(void) { return s_bc_on; }
const char *audio_broadcast_diag(void) { return s_bc_err; }
uint32_t audio_broadcast_enc_us(void) { return s_bc_enc_us; }

static void broadcast_push(const int32_t *out, int frames)
{
    if (!s_bc_on || !s_bc_ring) return;
    for (int f = 0; f < frames; f++) {
        if (s_bc_w - s_bc_r >= BC_FRAMES) s_bc_r++;   // ring full: drop oldest
        uint32_t idx = (s_bc_w % BC_FRAMES) * 2;
        s_bc_ring[idx] = (int16_t)(out[f * 2] >> 16);
        s_bc_ring[idx + 1] = (int16_t)(out[f * 2 + 1] >> 16);
        s_bc_w++;
    }
}

static void bc_put_le32(uint8_t *p, uint32_t v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void bc_put_le16(uint8_t *p, uint16_t v){ p[0]=v; p[1]=v>>8; }

static void broadcast_server_task(void *pv)
{
    s_bc_ring = heap_caps_malloc((size_t)BC_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *sbuf = malloc(1024 * 2 * sizeof(int16_t));
    if (!s_bc_ring || !sbuf) { ESP_LOGE("BCAST", "alloc failed"); vTaskDelete(NULL); return; }

    // WAIT for the network: initAudio (and this task) runs BEFORE initWifi, so
    // the TCP/IP stack isn't up yet — calling socket() early asserts (Invalid
    // mbox). isWiFiConnected() implies tcpip_adapter_init + GOT_IP are done.
    while (!isWiFiConnected()) vTaskDelay(pdMS_TO_TICKS(200));

    // streaming WAV header: 16-bit 44.1k stereo, max sizes so players keep going
    uint8_t wav[44] = {0};
    memcpy(wav, "RIFF", 4);      bc_put_le32(wav + 4, 0xFFFFFFFF);
    memcpy(wav + 8, "WAVE", 4);  memcpy(wav + 12, "fmt ", 4);
    bc_put_le32(wav + 16, 16);   bc_put_le16(wav + 20, 1);   bc_put_le16(wav + 22, 2);
    bc_put_le32(wav + 24, 44100); bc_put_le32(wav + 28, 44100 * 4);
    bc_put_le16(wav + 32, 4);    bc_put_le16(wav + 34, 16);
    memcpy(wav + 36, "data", 4); bc_put_le32(wav + 40, 0xFFFFFFFF);

    static const char HDR[] = "HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\n"
                              "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    static const char HDR_MP3[] = "HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\n"
                                  "icy-name: strampler live\r\nicy-br: 96\r\n"
                                  "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { ESP_LOGE("BCAST", "socket failed"); vTaskDelete(NULL); return; }
    int yes = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_port = htons(BC_PORT); sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(srv, 1) < 0) {
        ESP_LOGE("BCAST", "bind/listen failed"); close(srv); vTaskDelete(NULL); return;
    }
    ESP_LOGI("BCAST", "output broadcast on :%d (http://<ip>:%d/)", BC_PORT, BC_PORT);

    for (;;) {
        int cl = accept(srv, NULL, NULL);
        if (cl < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        // read the request line just far enough to route: ".mp3" -> encoder,
        // anything else (including a failed read) -> the original WAV stream
        bool want_mp3 = false;
        {
            struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
            setsockopt(cl, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char req[160];
            int rn = recv(cl, req, sizeof(req) - 1, 0);
            if (rn > 0) {
                req[rn] = 0;
                char *eol = strpbrk(req, "\r\n");
                if (eol) *eol = 0;
                want_mp3 = strstr(req, ".mp3") != NULL;
            }
        }

        s_bc_w = s_bc_r = 0;
        s_bc_on = true;

        if (want_mp3) {
            // MONO 96k: measured 2026-07-16, stereo\'s subband+MDCT is PSRAM-
            // cache-bound at ~34 ms per 26.1 ms pass (-O3 bought 3%) — mono
            // halves the DSP AND the hot working set; 96k also iterates the
            // quantizer less than 64k. The LAN WAV stream stays stereo.
            shine_config_t cfg;
            cfg.wave.channels = PCM_MONO;
            cfg.wave.samplerate = 44100;
            shine_set_config_mpeg_defaults(&cfg.mpeg);
            cfg.mpeg.mode = MONO;
            cfg.mpeg.bitr = 96;
            shine_t enc = shine_initialise(&cfg);
            if (!enc) {
                ESP_LOGE("BCAST", "shine init failed");
                s_bc_err = "shine-init";
                s_bc_on = false; close(cl); continue;
            }
            int pass = shine_samples_per_pass(enc);          // 1152 @ MPEG-1
            // PSRAM: internal RAM is NOT reliably available while radio's
            // helix path is live (bench-caught: pcm-alloc failed on 4.6 KB)
            int16_t *pcm = heap_caps_malloc((size_t)pass * sizeof(int16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!pcm) { ESP_LOGE("BCAST", "pcm alloc failed"); s_bc_err = "pcm-alloc"; }
            bool ok = pcm && send(cl, HDR_MP3, strlen(HDR_MP3), 0) > 0;
            if (pcm) s_bc_err = "ok";
            while (ok) {
                if (s_bc_w - s_bc_r < (uint32_t)pass) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
                for (int f = 0; f < pass; f++) {             // stereo bus -> mono mid
                    uint32_t idx = (s_bc_r % BC_FRAMES) * 2;
                    pcm[f] = (int16_t)(((int32_t)s_bc_ring[idx] + s_bc_ring[idx + 1]) >> 1);
                    s_bc_r++;
                }
                int written = 0;
                int64_t t0 = esp_timer_get_time();
                uint8_t *frame = shine_encode_buffer_interleaved(enc, pcm, &written);
                uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
                s_bc_enc_us = s_bc_enc_us ? (s_bc_enc_us * 7 + us) / 8 : us;
                if (written > 0 && send(cl, frame, written, 0) < 0) ok = false;
            }
            free(pcm);
            shine_close(enc);
        } else {
            bool ok = send(cl, HDR, strlen(HDR), 0) > 0 && send(cl, wav, 44, 0) > 0;
            while (ok) {
                int fr = 0;
                while (fr < 1024 && s_bc_r < s_bc_w) {
                    uint32_t idx = (s_bc_r % BC_FRAMES) * 2;
                    sbuf[fr * 2] = s_bc_ring[idx]; sbuf[fr * 2 + 1] = s_bc_ring[idx + 1];
                    s_bc_r++; fr++;
                }
                if (fr > 0) { if (send(cl, sbuf, fr * 4, 0) < 0) ok = false; }
                else vTaskDelay(pdMS_TO_TICKS(8));
            }
        }
        s_bc_on = false;
        close(cl);
    }
}

// ---- trig edge acquisition ----------------------------------------------------
// The audio task samples the trig pins once per block (0.726 ms), so a real
// ~1 ms eurorack gate and a floating-input glitch are indistinguishable at
// the sample level — no block-counted debounce can separate them (HANDOFF:
// TG_DEBOUNCE 2 drops short gates; 1 lets a floating TR2 toggle the loop).
// An edge ISR measures the true LOW width instead: gate-assert edges are
// timestamped and validated at >= TRIG_MIN_US, and the audio task publishes
// one machine_io_t.trig_rising bit per validated assert — a 1 ms gate can no
// longer fall between block samples, and sub-100 us noise never registers.
#define TRIG_MIN_US 200
static volatile int64_t s_trig_t_assert[2] = {0, 0};   // stamp of last assert edge
static volatile bool    s_trig_low[2] = {false, false}; // ISR's view of the line
static volatile bool    s_trig_pulse[2] = {false, false}; // completed valid pulse

static void IRAM_ATTR trig_isr(void *arg)
{
    int t = (int)(intptr_t)arg;
    int64_t now = esp_timer_get_time();
    if (gpio_get_level(t ? TRIG1_PIN : TRIG0_PIN) == 0) {   // assert (active low)
        s_trig_t_assert[t] = now;
        s_trig_low[t] = true;
    } else if (s_trig_low[t]) {                              // release: validate
        s_trig_low[t] = false;
        if (now - s_trig_t_assert[t] >= TRIG_MIN_US) s_trig_pulse[t] = true;
    }
}

// one rising bit per validated assert: sticky completed pulses OR a gate
// still held past the validation width; s_rep_t de-dupes by assert stamp
static uint8_t trig_rising_bits(void)
{
    static int64_t s_rep_t[2] = {-1, -1};
    int64_t now = esp_timer_get_time();
    uint8_t bits = 0;
    for (int t = 0; t < 2; t++) {
        int64_t ta = s_trig_t_assert[t];
        bool valid = s_trig_pulse[t] ||
                     (s_trig_low[t] && now - ta >= TRIG_MIN_US);
        s_trig_pulse[t] = false;
        if (valid && ta != s_rep_t[t]) {
            bits |= (uint8_t)(1 << t);
            s_rep_t[t] = ta;
        }
    }
    return bits;
}

static void audio_task(void *pvParams)
{
    int32_t out[BUF_SZ], in[BUF_SZ];
    size_t nb;
    uint16_t ctrlData[8];
    machine_io_t io = {0};

    for (;;)
    {
        // get control data
        xQueueReceive(control_queue, &ctrlData, 0);

        // machine I/O snapshot (teleremote CV overrides substitute for the
        // ADC while fresh — both views, so legacy cv_raw readers agree)
        TickType_t cvnow = xTaskGetTickCount();
        for (int i = 0; i < 8; i++) {
            if (cvnow < s_remote_cv_until[i]) {
                io.cv[i] = s_remote_cv_val[i];
                io.cv_raw[i] = s_remote_cv_val[i];
            } else {
                io.cv[i] = cv_corrected(i, ctrlData);
                io.cv_raw[i] = ctrlData[i];
            }
        }
        io.trig_level = (gpio_get_level(TRIG0_PIN) ? 1 : 0) | (gpio_get_level(TRIG1_PIN) ? 2 : 0);
        io.trig_rising = trig_rising_bits();
        // merge teleremote soft pulses (assert = pull low, like the jacks);
        // a soft assert edge also counts as a validated rising event
        static uint8_t s_soft_prev = 0;
        TickType_t now = xTaskGetTickCount();
        uint8_t soft = (now < s_remote_trig_until[0] ? 1 : 0) |
                       (now < s_remote_trig_until[1] ? 2 : 0);
        io.trig_level &= (uint8_t)~soft;
        io.trig_rising |= (uint8_t)(soft & ~s_soft_prev);
        s_soft_prev = soft;

        // v0/v1 names are pushed by the machine via audio_status_set_voices()
        portENTER_CRITICAL(&_status_mux);
        for (int i = 0; i < 8; i++) _audio_status.cv[i] = io.cv[i];
        _audio_status.trig = io.trig_level;
        portEXIT_CRITICAL(&_status_mux);

        // always read I2S to drain the RX buffer; route to machine or recording
        i2s_read(I2S_NUM_0, in, 256, &nb, portMAX_DELAY);
        if (recording_is_active() && !s_bounce)
            recording_push(in);
        // beat listener: core input tap like the recorder's, runs every block
        // regardless of machine (OFF = one branch). Runs BEFORE the machine so
        // this block's synthesized clock level is what the machine consumes.
        beatlisten_push(in);

        const machine_t *m = machine_active();
        if (m)
            m->process(out, in, &io);
        else
            memset(out, 0, sizeof(out));

        // BOUNCE tap: capture the machine's output HERE (before clock-out
        // overwrites a channel), so a bounce is the pure musical signal
        if (recording_is_active() && s_bounce)
            recording_push(out);

        // optional clock OUT: overwrite one channel with beat pulses
        beatlisten_out_render(out);

        // rough STEREO VU: decayed per-block peak per channel (16-bit
        // magnitude >> 7 => 0..255), order inL/inR/outL/outR. A "is signal
        // arriving / leaving, and on which side" meter, no more.
        {
            static uint8_t vu[4] = {0};
            int32_t pk[4] = {0};
            for (int i = 0; i < BUF_SZ; i += 2) {
                int32_t il = in[i] >> 16, ir = in[i + 1] >> 16;
                int32_t ol = out[i] >> 16, orr = out[i + 1] >> 16;
                if (il < 0) il = -il;
                if (ir < 0) ir = -ir;
                if (ol < 0) ol = -ol;
                if (orr < 0) orr = -orr;
                if (il > pk[0]) pk[0] = il;
                if (ir > pk[1]) pk[1] = ir;
                if (ol > pk[2]) pk[2] = ol;
                if (orr > pk[3]) pk[3] = orr;
            }
            portENTER_CRITICAL(&_status_mux);
            for (int k = 0; k < 4; k++) {
                uint8_t n = (uint8_t)(pk[k] >> 7 > 255 ? 255 : pk[k] >> 7);
                vu[k] = n > vu[k] ? n : (uint8_t)((vu[k] * 15) >> 4);   // fast up, decayed down
                _audio_status.vu[k] = vu[k];
            }
            portEXIT_CRITICAL(&_status_mux);
        }

        broadcast_push(out, BUF_SZ / 2);   // live WAV stream tap (the final output)
        i2s_write(I2S_NUM_0, out, BUF_SZ * 4, &nb, portMAX_DELAY);
    }
}

void initAudio(void)
{
    control_queue = xQueueCreate(10, sizeof(uint16_t) * 8);
    initSpiPer(control_queue);
    init_i2s();
    recording_init();
    beatlisten_init();
    // trig edge ISRs (service may already be installed by the encoder init)
    esp_err_t ie = gpio_install_isr_service(0);
    if (ie != ESP_OK && ie != ESP_ERR_INVALID_STATE)
        ESP_LOGE("AUDIO", "gpio isr service: %d", ie);
    gpio_set_intr_type(TRIG0_PIN, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(TRIG1_PIN, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(TRIG0_PIN, trig_isr, (void *)0);
    gpio_isr_handler_add(TRIG1_PIN, trig_isr, (void *)1);
    ESP_LOGI("AUDIO", "Starting audio task");
    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 23, &audio_task_h, 1);
}

// Start the output-broadcast socket server. MUST be called AFTER initWifi() —
// both socket() and isWiFiConnected() touch state that initWifi creates, and
// calling either earlier asserts (Invalid mbox / xEventGroup). Unpinned.
void audio_broadcast_init(void)
{
    xTaskCreate(broadcast_server_task, "bc_srv", 12288, NULL, 5, NULL);   // stack: shine encode runs here
}
