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
#include "menu_config.h"
#include "clock.h"          // core clock settings at boot
#include "freesound.h"
#include "mp3.h"
#include "wifi.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"     // redraw timing (ui_tft_stat)
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

// ---- Display redraw timing (redraw-speed work, 2026-09-09) -----------------
// Every draw funnels through menuProcessEvent inside ui_ev_loop, so timing the
// event there measures the whole repaint: PSRAM shadow mirror + SPI wire.
// Timer ticks (300 ms fast / 1000 ms slow) are tracked separately from
// user-driven events (a machine switch is a full repaint; a tick is the
// per-machine live update, the thing that makes the screen feel choppy).
// Read via /sysinfo "tft"; cleared with /sysinfo?tftclear=1.
static uint32_t s_tft_ev_us, s_tft_worst_us, s_tft_tick_us, s_tft_tick_worst_us;
static uint32_t s_tft_events;
static uint64_t s_tft_total_us;
static int      s_tft_worst_ev = 0;

// which: 0 last non-tick event us, 1 worst us (any), 2 last tick us,
//        3 worst tick us, 4 events counted, 5 average us, 6 event id of the worst
uint32_t ui_tft_stat(int which)
{
    switch (which) {
        case 0: return s_tft_ev_us;
        case 1: return s_tft_worst_us;
        case 2: return s_tft_tick_us;
        case 3: return s_tft_tick_worst_us;
        case 4: return s_tft_events;
        case 5: return s_tft_events ? (uint32_t)(s_tft_total_us / s_tft_events) : 0;
        case 6: return (uint32_t)s_tft_worst_ev;
        default: return 0;
    }
}

void ui_tft_stats_clear(void)
{
    s_tft_ev_us = s_tft_worst_us = s_tft_tick_us = s_tft_tick_worst_us = 0;
    s_tft_events = 0; s_tft_total_us = 0; s_tft_worst_ev = 0;
}

static inline void tft_stat_note(int ev, uint32_t dt)
{
    if (ev == EV_TIMER_REPEATING_FAST || ev == EV_TIMER_REPEATING_SLOW) {
        s_tft_tick_us = dt;
        if (dt > s_tft_tick_worst_us) s_tft_tick_worst_us = dt;
    } else {
        s_tft_ev_us = dt;
    }
    if (dt > s_tft_worst_us) { s_tft_worst_us = dt; s_tft_worst_ev = ev; }
    s_tft_total_us += dt;
    s_tft_events++;
}

// Display SPI write clock, live. settings.tftclk (MHz) is read at boot in
// configDisplay; POST /settings applies it here under the display lock so it
// never lands mid-transaction. Returns the clock actually set (Hz), 0 if refused.
uint32_t ui_tft_set_clock_hz(uint32_t hz)
{
    if (hz < 8000000 || hz > 80000000) return 0;
    disp_lock_take();
    uint32_t got = spi_lobo_set_speed(disp_spi, hz);
    disp_lock_give();
    return got;
}

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
            int64_t t_draw0 = esp_timer_get_time();
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
            tft_stat_note(ev.event, (uint32_t)(esp_timer_get_time() - t_draw0));
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
    // settings.tftclk (MHz, per unit; default = the library's 26). 40 is the
    // usual ILI9341 overclock; prove it on a unit with GET /tftread?pattern=1
    // (pixel readback, needs SJ1 bridged) before persisting it there.
    {
        int mhz = configGetIntSetting("tftclk", DEFAULT_SPI_CLOCK / 1000000);
        if (mhz < 8 || mhz > 80) mhz = DEFAULT_SPI_CLOCK / 1000000;
        spi_lobo_set_speed(spi, (uint32_t)mhz * 1000000u);
    }
    printf("SPI: Changed speed to %u\r\n", spi_lobo_get_speed(spi));
    TFT_setGammaCurve(DEFAULT_GAMMA_CURVE);
	TFT_setRotation(LANDSCAPE_FLIP);
	// NOTE: tft_shadow_init() is NOT called at boot anymore — its 230 KB PSRAM
	// claim starved libxmp (tracker "FAIL: no memory"). The /screenshot handler
	// allocates it lazily on first use and kicks a redraw to fill it.
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
    // settings.encres = quadrature counts per detent: 2 = prototype encoder
    // (rests at both states; default), 4 = the EC11-class parts on new units
    // (rest at one state — they double-step at 2). AFTER mountSDStorage().
    initGPIO(ui_ev_queue, configGetIntSetting("encres", 2));
    gpioSetEncoderDirection(configGetIntSetting("encdir", 0)); // 1 = reversed lot
    // the CORE clock's global settings (clock.h): source (CLK_SRC_*; 3 = CV4),
    // pulses per beat, internal BPM, auto-fallback to INT when unlocked
    clock_core_set_src(configGetIntSetting("clk_src", 3));
    clock_core_set_ppb((float)configGetIntSetting("clk_ppq", 4));
    clock_core_set_int_bpm((float)configGetIntSetting("clk_bpm", 120));
    clock_core_set_auto(configGetIntSetting("clk_auto", 0) != 0);

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
    audio_broadcast_set_enabled(configGetIntSetting("broadcast", 0));   // OFF by default (frees 12 KB internal for the tracker); AFTER initWifi. Toggle in System→Settings / web.
    freesoundInit(ui_ev_queue);
    initMP3Engine(ui_ev_queue);
    startRestAPI(ui_ev_queue);
}