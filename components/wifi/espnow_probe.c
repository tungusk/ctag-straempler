// ESP-NOW COEXISTENCE SPIKE (2026-07-28). Not a feature — a measurement.
//
// The plan for units talking to each other ad-hoc rests on ESP-NOW: connectionless,
// no AP, MAC-layer, and low jitter is what clock sync needs rather than bandwidth.
// Before building any of that, one question decides whether the approach is viable
// at all on this hardware: DOES ESP-NOW TRAFFIC DISTURB THE AUDIO?
//
// It is a fair worry. Ordinary HTTP requests to this module underran the I2S DMA
// on ~28% of requests until the buffer was deepened (see drivers/i2s_per.c), and
// that is the same radio and the same CPU. If ESP-NOW at sync rates costs audio,
// the whole direction needs rethinking early rather than after it is built.
//
// This file sends broadcast frames at a commanded rate and does nothing else. One
// unit is enough: the question is what TRANSMITTING costs locally, which needs no
// peer. Drive it from tools/bench and measure the analog output either side.
//
// Deliberately OFF at boot and driven only by espnow_probe_set_hz(). Nothing in
// the audio path references it.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "espnow_probe.h"

static const char *TAG = "ESPNOW";

static volatile int  s_hz = 0;         // 0 = idle
static volatile bool s_inited = false;
static volatile uint32_t s_sent = 0, s_failed = 0;
static TaskHandle_t s_task;

static const uint8_t BCAST[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// what a sync packet would plausibly look like: a counter and a timestamp. Size
// matters for airtime, so send something representative rather than one byte.
typedef struct {
    uint32_t seq;
    uint32_t tick;
    uint8_t  pad[24];
} probe_pkt_t;

static void send_cb(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac;
    if (status == ESP_NOW_SEND_SUCCESS) s_sent++;
    else s_failed++;
}

static void probe_task(void *arg)
{
    (void)arg;
    probe_pkt_t p = {0};
    for (;;) {
        int hz = s_hz;
        if (hz <= 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        p.seq++;
        p.tick = (uint32_t)xTaskGetTickCount();
        esp_now_send(BCAST, (const uint8_t *)&p, sizeof(p));
        int period_ms = 1000 / hz;
        if (period_ms < 1) period_ms = 1;
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
}

int espnow_probe_set_hz(int hz)
{
    if (hz < 0) hz = 0;
    if (hz > 500) hz = 500;

    if (hz > 0 && !s_inited) {
        // esp_now_init REQUIRES the wifi driver to be started — call only after
        // initWifi (the network-tasks-after-initWifi rule this project already
        // learned the hard way with mdns and sntp).
        if (esp_now_init() != ESP_OK) { ESP_LOGE(TAG, "esp_now_init failed"); return -1; }
        esp_now_register_send_cb(send_cb);
        // The broadcast peer must sit on the channel the STA is already using —
        // ESP-NOW shares the radio, it does not get its own. channel 0 means
        // "whatever the interface is on", which is what makes this coexist with
        // the AP connection instead of fighting it.
        esp_now_peer_info_t peer = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false };
        memcpy(peer.peer_addr, BCAST, ESP_NOW_ETH_ALEN);
        if (esp_now_add_peer(&peer) != ESP_OK) { ESP_LOGE(TAG, "add_peer failed"); return -1; }
        if (xTaskCreate(probe_task, "espnow_probe", 3072, NULL, 5, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "task create failed");
            return -1;
        }
        s_inited = true;
        ESP_LOGI(TAG, "esp-now up");
    }
    s_hz = hz;
    return 0;
}

void espnow_probe_stats(int *hz, unsigned *sent, unsigned *failed)
{
    if (hz)     *hz     = s_hz;
    if (sent)   *sent   = s_sent;
    if (failed) *failed = s_failed;
}

void espnow_probe_reset_stats(void) { s_sent = 0; s_failed = 0; }
