#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>
#include "ui.h"
#include "ui_events.h"
#include "storage.h"
#include "gpio.h"
#include "pin_defs.h"
#include "tft.h"
#include "disp_lock.h"
#include "menu.h"
#include "freesound.h"
#include "mp3.h"
#include "wifi.h"
#include "esp_ota_ops.h"
#include "c_timeutils.h"
#include "timer_utils.h"
#include "audio.h"
#include "rest-api.h"

#define SPI_BUS TFT_HSPI_HOST

static TaskHandle_t *ui_task;
static xQueueHandle ui_ev_queue = NULL;
static xQueueHandle ui_param_queue_v1 = NULL;
static xQueueHandle ui_param_queue_v0 = NULL;
static xQueueHandle effect_param_queue = NULL;
static xQueueHandle pbs_state_queue_v0 = NULL;
static xQueueHandle pbs_state_queue_v1 = NULL;
static xQueueHandle mode_queue_v0 = NULL;
static xQueueHandle mode_queue_v1 = NULL;
static xQueueHandle matrix_event_queue = NULL;

xQueueHandle uiGetEventQueue(void){ return ui_ev_queue; }

static void ui_ev_loop(void* pvParams)
{
    ui_handler_param_t *params = pvParams;
    xQueueHandle ui_evt_queue = params->ui_evt_queue;
    ui_ev_ts_t ev;
    struct timeval lastBtnEv;
    uint32_t last = 0;  

    gettimeofday(&lastBtnEv, NULL);

    for(;;) {
        if(xQueueReceive(ui_evt_queue, &ev, portMAX_DELAY)) {

            static ui_ev_t btn_state = EV_NONE, btn_serviced = 0;
            // Every draw funnels through menuProcessEvent below; hold the display
            // bus for the whole event so a /screenshot readback can't interleave
            // SPI transactions with a draw (released between events — a capture
            // waits at most one event).
            disp_lock_take();
            switch(ev.event){
                case EV_ENC1_FWD:
                    menuProcessEvent(EV_FWD, NULL);
                    break;
                case EV_ENC1_BWD:
                    menuProcessEvent(EV_BWD, NULL);
                    break;
                case EV_ENC1_BT_DWN:
                    last = timeval_durationBeforeNow(&lastBtnEv);
                    //ESP_LOGI("UI", "DOWN %u", last);
                    if(last < 120 || btn_state == EV_ENC1_BT_DWN) break;
                    gettimeofday(&lastBtnEv, NULL);
                    btn_state = EV_ENC1_BT_DWN;
                    btn_serviced = 0;
                    setTimerSingleShot(500, ui_ev_queue);
                    break;
                case EV_ENC1_BT_UP:
                    last = timeval_durationBeforeNow(&lastBtnEv);
                    //ESP_LOGI("UI", "UP %u", last);
                    if(last < 20 || btn_state == EV_ENC1_BT_UP) break;
                    gettimeofday(&lastBtnEv, NULL);
                    btn_state = EV_ENC1_BT_UP;
                    if(!btn_serviced){
                        btn_serviced = 1;
                        // Fall back to duration at release: if the 500 ms one-shot
                        // timer was delayed (its service task is low priority and
                        // can be starved by WiFi), a genuine hold would otherwise
                        // be mis-serviced as a short press. Measure the real hold.
                        if(last >= 500) menuProcessEvent(EV_LONG_PRESS, NULL);
                        else            menuProcessEvent(EV_SHORT_PRESS, NULL);
                    }
                    break;
                case EV_TIMER_ONE_SHOT:
                    // ESP_LOGI("UI", "TIMER ONE SHOT, serviced: %d", btn_serviced);
                    if(!btn_serviced){
                        btn_serviced = 1;
                        menuProcessEvent(EV_LONG_PRESS, NULL);
                    }
                    menuProcessEvent(EV_TIMER_COMPLETE, NULL);
                    break;
                default:
                    menuProcessEvent(ev.event, ev.event_data);
                    break;
            }
            disp_lock_give();
        }
    }
    vTaskDelete(NULL);
}

void configDisplay(){
    esp_err_t ret;
    // set up TFT
    TFT_PinsInit();
    // ====  CONFIGURE SPI DEVICES(s)  ====================================================================================

    spi_lobo_device_handle_t spi;
    
    spi_lobo_bus_config_t buscfg={
        .miso_io_num=SPI_TFT_MISO_PIN,				// set SPI MISO pin
        .mosi_io_num=SPI_TFT_MOSI_PIN,				// set SPI MOSI pin
        .sclk_io_num=SPI_TFT_SCK_PIN,				// set SPI CLK pin
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
        .max_transfer_sz = 6*1024,
    };
    spi_lobo_device_interface_config_t devcfg={
        .clock_speed_hz=8000000,                // Initial clock out at 8 MHz
        .mode=0,                                // SPI mode 0
        .spics_io_num=-1,                       // we will use external CS pin
        .spics_ext_io_num=SPI_TFT_CS_PIN,           // external CS pin
        .flags=LB_SPI_DEVICE_HALFDUPLEX,           // ALWAYS SET  to HALF DUPLEX MODE!! for display spi
    };
    vTaskDelay(500 / portTICK_RATE_MS);
    // ==== Initialize the SPI bus and attach the LCD to the SPI bus ====

	ret=spi_lobo_bus_add_device(SPI_BUS, &buscfg, &devcfg, &spi);
    assert(ret==ESP_OK);
	printf("SPI: display device added to spi bus (%d)\r\n", SPI_BUS);
	disp_spi = spi;

	// ==== Test select/deselect ====
	ret = spi_lobo_device_select(spi, 1);
    assert(ret==ESP_OK);
	ret = spi_lobo_device_deselect(spi);
    assert(ret==ESP_OK);

	printf("SPI: attached display device, speed=%u\r\n", spi_lobo_get_speed(spi));
    printf("SPI: bus uses native pins: %s\r\n", spi_lobo_uses_native_pins(spi) ? "true" : "false");
    
    printf("SPI: display init...\r\n");
    TFT_display_init();
    printf("OK\r\n");
	
	// ---- Detect maximum read speed ----
	max_rdclock = find_rd_speed();
	printf("SPI: Max rd speed = %u\r\n", max_rdclock);

    // ==== Set SPI clock used for display operations ====
	spi_lobo_set_speed(spi, DEFAULT_SPI_CLOCK);
    printf("SPI: Changed speed to %u\r\n", spi_lobo_get_speed(spi));
    TFT_setGammaCurve(DEFAULT_GAMMA_CURVE);
	TFT_setRotation(LANDSCAPE_FLIP);
	tft_shadow_init();                 // shadow FB after orientation is final (/screenshot)
	TFT_setFont(DEFAULT_FONT, NULL);
    TFT_resetclipwin();
    _fg = TFT_CYAN;
    //TFT_print("Freesound Sampler", CENTER, CENTER); 
    struct stat st = {0};
    if (stat("/sdcard/bootlogo.bmp", &st) != -1) {
        //ESP_LOGE("Boot", "logo file %s", "/sdcard/bootlogo.bmp");
        TFT_bmp_image(CENTER, CENTER, 0, "/sdcard/bootlogo.bmp", NULL, 0);
        vTaskDelay(3000 / portTICK_RATE_MS);
    }
    
}

static void timerRepeatSlow(){
    ui_ev_ts_t ev;
    for(;;){
        //ESP_LOGI("", "Tick");
        ev.event = EV_TIMER_REPEATING_SLOW;
        xQueueSend(ui_ev_queue, &ev, portMAX_DELAY);
        vTaskDelay(1000 / portTICK_RATE_MS);
    }
    vTaskDelete(NULL);
}

static void timerRepeatFast(){
    ui_ev_ts_t ev;
    for(;;){
        //ESP_LOGI("", "Tick");
        ev.event = EV_TIMER_REPEATING_FAST;
        xQueueSend(ui_ev_queue, &ev, portMAX_DELAY);
        vTaskDelay(300 / portTICK_RATE_MS);
    }
    vTaskDelete(NULL);
}

// OTA rollback: a freshly-pushed image boots PENDING_VERIFY. Mark it valid only
// once WiFi (the OTA lifeline) is up + a few seconds of stable runtime — so a
// bad image that crashes early or can't reach the network auto-reverts to the
// last good slot on the next reset instead of stranding the device with no OTA
// path. Requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (bootloader-side).
static void otaValidateTask(void *arg){
    (void)arg;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY){
        for (int i = 0; i < 60 && !isWiFiConnected(); i++) vTaskDelay(pdMS_TO_TICKS(500));   // wait up to 30s
        if (isWiFiConnected()){
            vTaskDelay(pdMS_TO_TICKS(4000));                    // prove a few seconds of stable runtime
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
                ESP_LOGI("OTA", "image marked VALID (rollback cancelled)");
        } else {
            ESP_LOGW("OTA", "WiFi never came up -> image left unverified (rolls back on reset)");
        }
    }
    vTaskDelete(NULL);
}

void initUI(){
    ui_ev_queue = xQueueCreate(64, sizeof(ui_ev_ts_t));
    ui_handler_param_t *params = calloc(1, sizeof(ui_handler_param_t));
    params->ui_evt_queue = ui_ev_queue;
    params->user_data = NULL;

    mountSDStorage();
    configDisplay();
    initGPIO(ui_ev_queue);

    initAudio();
    initMenu(ui_ev_queue);

    //xTaskCreatePinnedToCore(ui_task, "ui_task", usStackDepth, params, 10, gpio_task, 1);
    disp_lock_init();   // guard the TFT/SPI bus before drawing goes multi-task (screenshot reader)
    xTaskCreatePinnedToCore(ui_ev_loop, "ui_ev_loop", 4096*2, params, 11, ui_task, 0);
    
    //initGPIO(ui_ev_queue, au_q);
    xTaskCreatePinnedToCore(timerRepeatSlow, "timerRepeatSlow", 2048, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(timerRepeatFast, "timerRepeatFast", 2048, NULL, 10, NULL, 0);
    
    initWifi();
    xTaskCreate(otaValidateTask, "ota_validate", 3072, NULL, 3, NULL);   // commit or roll back this OTA image
    audio_broadcast_init();   // output-broadcast socket server — AFTER initWifi (needs tcpip + the wifi event group)
    freesoundInit(ui_ev_queue);
    initMP3Engine(ui_ev_queue);
    startRestAPI(ui_ev_queue);
}