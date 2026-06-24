#include "soc/rtc_cntl_reg.h"
#include "soc/rtc_io_reg.h"
#include "soc/rtc.h"
#include "driver/rtc_io.h"
#include "driver/rtc_cntl.h"
#include "esp32/ulp.h"
#include "ulp_drivers.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdbool.h>

extern void ets_delay_us(uint32_t us); // ESP32 ROM function, no header needed


extern const uint8_t ulp_drivers_bin_start[] asm("_binary_ulp_drivers_bin_start");
extern const uint8_t ulp_drivers_bin_end[]   asm("_binary_ulp_drivers_bin_end");

const gpio_num_t GPIO_CS1 = GPIO_NUM_4;
const gpio_num_t GPIO_CS0 = GPIO_NUM_32;
const gpio_num_t GPIO_MOSI = GPIO_NUM_13;
const gpio_num_t GPIO_SCLK = GPIO_NUM_12;
const gpio_num_t GPIO_MISO = GPIO_NUM_0;

static QueueHandle_t adcDataQueue = NULL;

static void init_ulp_program()
{
    ESP_ERROR_CHECK( ulp_load_binary(0, ulp_drivers_bin_start,(ulp_drivers_bin_end - ulp_drivers_bin_start) / sizeof(uint32_t)));

    rtc_gpio_init(GPIO_CS0);
    rtc_gpio_set_direction(GPIO_CS0, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_CS0, 0);

    rtc_gpio_init(GPIO_CS1);
    rtc_gpio_set_direction(GPIO_CS1, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_CS1, 0);

    rtc_gpio_init(GPIO_MOSI);
    rtc_gpio_set_direction(GPIO_MOSI, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_MOSI, 0);

    rtc_gpio_init(GPIO_SCLK);
    rtc_gpio_set_direction(GPIO_SCLK, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_SCLK, 0);

    rtc_gpio_init(GPIO_MISO);
    rtc_gpio_set_direction(GPIO_MISO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(GPIO_MISO);
    
}

static void IRAM_ATTR ulp_isr(void* arg)
{
    uint16_t data[8];
    uint16_t offs;
    /* hardware debugging 
    gpio_set_level(SPI_TFT_MISO_PIN, 1);
    */
    offs = (uint16_t)*((&ulp_adc_data_offset));
    // swap CV1/CV2 due to PCB routing
    data[1] = (uint16_t)*((&ulp_adc_data) + 0 + offs);
    data[0] = (uint16_t)*((&ulp_adc_data) + 1 + offs);
    // end swap
    data[2] = (uint16_t)*((&ulp_adc_data) + 2 + offs);
    data[3] = (uint16_t)*((&ulp_adc_data) + 3 + offs);
    data[4] = (uint16_t)*((&ulp_adc_data) + 4 + offs);
    // swap these due to logic on PCB / UI layout
    data[6] = (uint16_t)*((&ulp_adc_data) + 5 + offs);
    data[5] = (uint16_t)*((&ulp_adc_data) + 6 + offs);
    // end swap
    data[7] = (uint16_t)*((&ulp_adc_data) + 7 + offs);
    
    xQueueSendFromISR( adcDataQueue, data, NULL );
    /* hardware debugging 
    gpio_set_level(SPI_TFT_MISO_PIN, 0);
    */
}


void blink_task(void *pvParameter)
{
    uint16_t v[8];
    while(1) {
        xQueueReceive( adcDataQueue, &v,  portMAX_DELAY); 
        ESP_LOGI("DATA", "%d, %d, %d, %d, %d, %d, %d, %d", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    }
}


// RTC GPIO bit indices matching spi-bb.S macros — verified by ULP hardware
#define WM_CS_RTC   10   // GPIO_NUM_4
#define WM_MOSI_RTC 14   // GPIO_NUM_13
#define WM_SCLK_RTC 13   // RTC GPIO 13 (actual SCLK pin per ULP assembly)

#define _wm_lo(n) REG_WRITE(RTC_GPIO_OUT_W1TC_REG, 1U << (RTC_GPIO_OUT_DATA_W1TC_S + (n)))
#define _wm_hi(n) REG_WRITE(RTC_GPIO_OUT_W1TS_REG, 1U << (RTC_GPIO_OUT_DATA_W1TS_S + (n)))

static void codec_write_reg(uint16_t word)
{
    _wm_lo(WM_SCLK_RTC);
    _wm_lo(WM_MOSI_RTC);
    _wm_lo(WM_CS_RTC);            // CS LOW = assert WM8731
    for (int i = 15; i >= 0; i--) {
        _wm_lo(WM_SCLK_RTC);
        if ((word >> i) & 1) _wm_hi(WM_MOSI_RTC);
        else                 _wm_lo(WM_MOSI_RTC);
        _wm_hi(WM_SCLK_RTC);      // rising edge latches bit
    }
    _wm_lo(WM_SCLK_RTC);
    _wm_hi(WM_CS_RTC);            // CS HIGH = deassert
}

void codec_set_input(bool use_mic)
{
    // WM8731 R4 (Analog Audio Path Control):
    //   line in: BYPASS=1 (bit4), INSEL=0 (bit3) → data=0x10  [ULP default]
    //   mic in:  INSEL=1 (bit3), MICBOOST=1 (bit1) → data=0x0A
    uint16_t data = use_mic ? 0x0Au : 0x10u;
    uint16_t word = (uint16_t)((4u << 9) | data);
    codec_write_reg(word);
    ets_delay_us(5);
    codec_write_reg(word);   // write twice — ULP may interfere on first
    ESP_LOGI("CODEC", "Input: %s", use_mic ? "mic" : "line");
}

void initSpiPer(xQueueHandle queue)
{
    esp_err_t err;
    adcDataQueue = queue;
    //adcDataQueue = xQueueCreate(1, sizeof(uint16_t)*8);
    err = rtc_isr_register(&ulp_isr, 0, RTC_CNTL_SAR_INT_ST_M);
    ESP_ERROR_CHECK(err);
    REG_SET_BIT(RTC_CNTL_INT_ENA_REG, RTC_CNTL_ULP_CP_INT_ENA_M);
    //xTaskCreate(&blink_task, "blink_task", 4096, NULL, 5, NULL);
    //rtc_clk_slow_freq_set(RTC_SLOW_FREQ_RTC);
    rtc_clk_fast_freq_set(RTC_FAST_FREQ_XTALD4);
    // set RTC FAST CLOCK to XTAL/4 = 10MHz at 40Mhz XTAL speed
    //ESP_LOGI("RTC CLOCK", "Frequency %d", rtc_clk_fast_freq_get());
    init_ulp_program();
    ESP_ERROR_CHECK( ulp_run((&ulp_entry - RTC_SLOW_MEM) / sizeof(uint32_t)));
}
