#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include <time.h>
#include <sys/time.h>
#include "gpio.h"
#include "pin_defs.h"
#include "ui_events.h"

#define ESP_INTR_FLAG_DEFAULT 0

static xQueueHandle ui_event_queue = NULL;

// Quadrature counts per detent (CONFIG.JSN settings.encres): 2 = prototype
// encoder resting at BOTH quadrature states (00 and 11), 4 = standard
// EC11-class part resting at one state (full cycle per detent). Set in
// initGPIO() before the ISRs are armed; POST /settings re-applies it live.
static volatile uint8_t s_enc_cpd = 2;

void gpioSetEncoderResolution(int enc_cpd)
{
    s_enc_cpd = (enc_cpd >= 4) ? 4 : 2;
}

static void IRAM_ATTR gpio_isr_handler_encoder1(void* arg)
{
    // Quadrature state machine, one event per detent (see s_enc_cpd above);
    // the old decoder fired only on the 00 arrival and ate every second
    // click of the half-cycle encoder. Invalid/bounce transitions score 0
    // or cancel, so the accumulator also debounces.
    static const int8_t qdec[16] = { 0, +1, -1,  0,
                                    -1,  0,  0, +1,
                                    +1,  0,  0, -1,
                                     0, -1, +1,  0 };
    static uint8_t prev = 3;
    static int8_t accum = 0;
    ui_ev_ts_t ev;
    ev.event_data = NULL;
    uint8_t ab = ((uint8_t)gpio_get_level(ENC_A_PIN) << 1) | (uint8_t)gpio_get_level(ENC_B_PIN);
    accum += qdec[(prev << 2) | ab];
    prev = ab;
    if(ab == 0 || ab == 3){ // rest state reached
        if(accum >= (int8_t)s_enc_cpd){
            ev.event = EV_ENC1_FWD;
            xQueueSendFromISR(ui_event_queue, &ev, NULL);
            accum = 0;
        }else if(accum <= -(int8_t)s_enc_cpd){
            ev.event = EV_ENC1_BWD;
            xQueueSendFromISR(ui_event_queue, &ev, NULL);
            accum = 0;
        }else if(s_enc_cpd == 2){
            accum = 0; // every rest is a detent; discard bounce residue
        }
        // cpd 4: the mid-cycle rest (accum +/-2) is not a detent — keep
        // accumulating toward the real one instead of eating the half-turn
    }
    return;
}

static void IRAM_ATTR gpio_isr_handler_encoder1_btn1(void* arg)
{
    ui_ev_ts_t ev;
    static ui_ev_t state = EV_ENC1_BT_UP;
    uint32_t pv = gpio_get_level(ENC_BTN_PIN);
    // de-bounce
    if(pv == 0 && state == EV_ENC1_BT_DWN) return;
    if(pv == 1 && state == EV_ENC1_BT_UP) return;
    // real event
    if(pv == 1 && state == EV_ENC1_BT_DWN) ev.event = EV_ENC1_BT_UP;
    if(pv == 0 && state == EV_ENC1_BT_UP) ev.event = EV_ENC1_BT_DWN;
    state = ev.event;
    xQueueSendFromISR(ui_event_queue, &ev, NULL);
}

void initGPIO(xQueueHandle queueui, int enc_cpd){
    // set queues
    ui_event_queue = queueui;
    gpioSetEncoderResolution(enc_cpd);
    
    gpio_config_t io_conf;

    //interrupt of rising edge
    io_conf.intr_type = GPIO_PIN_INTR_NEGEDGE;
    //bit mask of the pins
    io_conf.pin_bit_mask = ((1ULL<<ENC_A_PIN) | (1ULL<<ENC_B_PIN) | (1ULL<<ENC_BTN_PIN));
    //set as input mode    
    io_conf.mode = GPIO_MODE_INPUT;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    //change gpio intrrupt type for one pin
    gpio_set_intr_type(ENC_BTN_PIN, GPIO_INTR_ANYEDGE);
    // quadrature decoder needs both edges of both encoder pins
    gpio_set_intr_type(ENC_A_PIN, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(ENC_B_PIN, GPIO_INTR_ANYEDGE);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for encoder 1
    gpio_isr_handler_add(ENC_A_PIN, gpio_isr_handler_encoder1, (void*) ENC_A_PIN);
    gpio_isr_handler_add(ENC_B_PIN, gpio_isr_handler_encoder1, (void*) ENC_B_PIN);
    //hook isr handler for encoder 1 button
    gpio_isr_handler_add(ENC_BTN_PIN, gpio_isr_handler_encoder1_btn1, (void*) ENC_BTN_PIN);

    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = ((1ULL<<TRIG0_PIN)| (1ULL<<TRIG1_PIN));
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    /* debug pin
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = 1ULL<<SPI_TFT_MISO_PIN;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);
    */
}
