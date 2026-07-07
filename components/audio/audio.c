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
#include "audio.h"
#include "recording.h"
#include "machine.h"
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
    strncpy(_audio_status.v1, v1 ? v1 : "", 31); _audio_status.v1[31] = 0;
    portEXIT_CRITICAL(&_status_mux);
}

// teleremote soft triggers: each entry holds the tick until which that gate
// is held asserted; the audio task ANDs them into the hardware reading
static volatile TickType_t s_remote_trig_until[2] = {0, 0};

void audio_remote_trig(int t, int ms) {
    if (t < 0 || t > 1) return;
    s_remote_trig_until[t] = xTaskGetTickCount() + pdMS_TO_TICKS(ms > 0 ? ms : 30);
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

        // machine I/O snapshot
        for (int i = 0; i < 8; i++) {
            io.cv[i] = cv_corrected(i, ctrlData);
            io.cv_raw[i] = ctrlData[i];
        }
        io.trig_level = (gpio_get_level(TRIG0_PIN) ? 1 : 0) | (gpio_get_level(TRIG1_PIN) ? 2 : 0);
        // merge teleremote soft pulses (assert = pull low, like the jacks)
        TickType_t now = xTaskGetTickCount();
        if (now < s_remote_trig_until[0]) io.trig_level &= ~1;
        if (now < s_remote_trig_until[1]) io.trig_level &= ~2;

        // v0/v1 names are pushed by the machine via audio_status_set_voices()
        portENTER_CRITICAL(&_status_mux);
        for (int i = 0; i < 8; i++) _audio_status.cv[i] = io.cv[i];
        _audio_status.trig = io.trig_level;
        portEXIT_CRITICAL(&_status_mux);

        // always read I2S to drain the RX buffer; route to machine or recording
        i2s_read(I2S_NUM_0, in, 256, &nb, portMAX_DELAY);
        if (recording_is_active())
            recording_push(in);

        const machine_t *m = machine_active();
        if (m)
            m->process(out, in, &io);
        else
            memset(out, 0, sizeof(out));

        i2s_write(I2S_NUM_0, out, BUF_SZ * 4, &nb, portMAX_DELAY);
    }
}

void initAudio(void)
{
    control_queue = xQueueCreate(10, sizeof(uint16_t) * 8);
    initSpiPer(control_queue);
    init_i2s();
    recording_init();
    ESP_LOGI("AUDIO", "Starting audio task");
    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 23, &audio_task_h, 1);
}
