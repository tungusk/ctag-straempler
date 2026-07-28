#include "sdkconfig.h"
#include "i2s_per.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s.h"
#include "pin_defs.h"

void init_i2s(){
    static const i2s_config_t i2s_config = {
        .mode = I2S_MODE_SLAVE | I2S_MODE_TX | I2S_MODE_RX,
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, // default interrupt priority would be 0
        // DMA DEPTH = dma_buf_count * dma_buf_len frames of slack before the
        // hardware runs dry. It was 4 x 32 = 128 frames = 2.9 ms, which is less
        // slack than a single HTTP request needs: measured 2026-07-27 on the
        // analog output, roughly 28% of requests to the module produced an
        // audible phase discontinuity — the waveform jumps and carries on, the
        // signature of a repeated or dropped DMA buffer. A ping flood does NOT
        // do it (so it is not the radio, and not the size of the TX burst), and
        // large pings do not either; it is httpd's request handling occupying
        // the CPU for longer than the buffer can cover. With the web UI polling
        // /status twice a second that is about one glitch every two seconds.
        //
        // 12 x 32 = 384 frames = 8.7 ms buys ~6 ms more slack. The cost is
        // output latency, which for a module played from CV/gate is not
        // something you can feel at this scale. NOTE `aus`/`ausgap` cannot see
        // this class of fault: i2s_write uses portMAX_DELAY, so after an
        // underrun the DMA is empty, the write returns immediately, and the loop
        // simply catches up — the block-to-block interval barely moves even
        // though audio was lost.
        .dma_buf_count = 12,
        .dma_buf_len = 32,
        .use_apll = true
    };

    static const i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_LRCLK_PIN,
        .data_out_num = I2S_DACDAT_PIN,
        .data_in_num = I2S_ADCDAT_PIN
    };


    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);   //install and start i2s driver

    i2s_set_pin(I2S_NUM_0, &pin_config);

    //i2s_set_clk(I2S_NUM_0, 44100, I2S_BITS_PER_SAMPLE_16BIT, I2S_BITS_PER_SAMPLE_16BIT);

    //i2s_driver_uninstall(i2s_num); //stop & destroy i2s driver
}