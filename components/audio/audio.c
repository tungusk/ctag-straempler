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

        // machine I/O snapshot
        for (int i = 0; i < 8; i++) {
            io.cv[i] = cv_corrected(i, ctrlData);
            io.cv_raw[i] = ctrlData[i];
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
        if (recording_is_active())
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

        // optional clock OUT: overwrite one channel with beat pulses
        beatlisten_out_render(out);

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
