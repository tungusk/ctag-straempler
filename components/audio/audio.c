#include "audio.h"
#include "audio_luts.h"
#include "granular.h"
#include <string.h>
#include <stdatomic.h>
#include "driver/i2s.h"
#include "cJSON.h"
#include "fileio.h"
#include "spi_per.h"
#include "i2s_per.h"
#include "driver/gpio.h"
#include "pin_defs.h"
#include <stdio.h>
#include "math.h"
#include "sampler_fifo.h"
#include "esp_heap_caps.h"
#include "audio_events.h"
#include "audio_utils.h"
#include "ui_events.h"
#include "delay.h"
#include "fixed.h"
#include "menu_items.h"
#include "adsr.h"
#include "modulation.h"
#include "recording.h"

#define DIR_FWD 0
#define DIR_BWD 1
#define BIT_VOICE0 0x01
#define BIT_VOICE1 0x02
#define BIT_VOICE_0_TRIG 0x06
#define BIT_VOICE_1_TRIG 0xC
#define BIT_VOICE0_RETRIG 0x08
#define BIT_VOICE1_RETRIG 0x10
#define BIT_CONTROL_DATA 0x04
#define BIT_VOICE0_SEEK   0x20
#define BIT_VOICE1_SEEK   0x40
#define BIT_GRAIN0_LOAD   0x01
#define BIT_GRAIN1_LOAD   0x02
#define SAMPLE_RATE 44100
#define FRACTION_MASK 0x1ff
#define BUF_SZ 64
#define DELAY_MAX_LENGTH_MS 1500.0
// Using Q16 Unsigned Fixed Point Notation for ENV_LUT
#define LUT_SCALE 16
#define MUL_ENV(x, y) (((int)(x) * (y)) >> LUT_SCALE)
#define FixedToDouble2(x) ((double)(x) / (1 << 9))
#define DIVISOR 0.0000000004656612873077392578125f // is 1.0f/2^31

static audio_f_t audio_files[2];
static audio_b_t audio_buffers[6];

static audio_status_t _audio_status;
static portMUX_TYPE _status_mux = portMUX_INITIALIZER_UNLOCKED;

void audio_get_status(audio_status_t *out) {
    portENTER_CRITICAL(&_status_mux);
    *out = _audio_status;
    portEXIT_CRITICAL(&_status_mux);
}
static voice_t voice[2];
static fifo_t voice_fifos[2];
static matrix_row_t matrix[9];
static ui_param_holder_t ui_params[2];
static void (*play_modes[])(void *, void *, void *) = {fill_buffer_one_shot, fill_buffer_loop, fill_buffer_pipo};
static granular_t gran[2];

int controlData[3][8];

static xQueueHandle control_queue = NULL;
static xQueueHandle ui_parameter_queue_v0 = NULL;
static xQueueHandle ui_parameter_queue_v1 = NULL;
static xQueueHandle effect_parameter_queue = NULL;
static xQueueHandle playbackspeed_state_queue_v0 = NULL;
static xQueueHandle playbackspeed_state_queue_v1 = NULL;
static xQueueHandle mode_handle_v0 = NULL;
static xQueueHandle mode_handle_v1 = NULL;
static xQueueHandle m_event_queue = NULL;
static xQueueHandle ui_ev_queue = NULL;

// reload task handles
static TaskHandle_t file_reader_task_1_handle = NULL;
static TaskHandle_t file_reader_task_2_handle = NULL;
static TaskHandle_t grain_loader_task_handle  = NULL;
static TaskHandle_t audio_task_h;
static SemaphoreHandle_t counter_mutex[2] = {NULL};
static SemaphoreHandle_t file_mutex[2] = {NULL};
static SemaphoreHandle_t buffer_mutex[2] = {NULL};
static uint32_t b1 = 0, b2 = 0;
TaskHandle_t file_manipulation_task_handle = NULL;

// trig mode, usin stdatomic here as the variable may be changed from another thread / core
atomic_bool trigModeLatch[2] = {false, false};

void trigger_buffer_reload(int vid, TaskHandle_t *handle)
{
    xTaskNotify(*handle, 6 << vid, eSetBits);
}

static void file_reader_task_1(void *pvParams)
{
    uint32_t uxBits;
    ui_ev_ts_t ev;
    
    for (;;)
    {
        // wait for need to switch buffers
        xTaskNotifyWait(0x00,           /* Don't clear any notification bits on entry. */
                        ULONG_MAX,      /* Reset the notification value to 0 on exit. */
                        &uxBits,        /* Notified value pass out in
                                              ulNotifiedValue. */
                        portMAX_DELAY); /* Block indefinitely. */

        // switch audio buffers
        //

        if (uxBits & BIT_VOICE0)
        {
            xSemaphoreTake(buffer_mutex[0], portMAX_DELAY);
            xSemaphoreTake(counter_mutex[0], portMAX_DELAY);
            voice[0].voice_buffer = &audio_buffers[(b1++ % 2) + 1];
            int buf_num = ((b1) % 2) + 1;
            xSemaphoreGive(counter_mutex[0]);
            xSemaphoreGive(buffer_mutex[0]);  
            ev.event = EV_UPDATE_V0_POS;
            ev.event_data = (void*) (audio_files[0].fpos / (audio_files[0].fsize / 300));// 300 is from bar width in visualization
            xQueueSend(ui_ev_queue, &ev, 0); 
            fill_audio_buffer(&voice[0].playback_engine, &audio_buffers[buf_num], &audio_files[0], &file_mutex[0]);     
        }

        // refill buffers when sample is retriggered
        if (uxBits & BIT_VOICE0_RETRIG)
        {
            xSemaphoreTake(counter_mutex[0], portMAX_DELAY);
            b1 = 0;
            xSemaphoreGive(counter_mutex[0]);
            xSemaphoreTake(buffer_mutex[0], portMAX_DELAY);
            voice[0].voice_buffer = &audio_buffers[0];
            voice[0].voice_buffer->rpos = 0;
            xSemaphoreGive(buffer_mutex[0]);
            jump_to_start(&voice[0].playback_engine, &audio_buffers[0], &audio_buffers[1], &audio_files[0], &file_mutex[0]);
        }

        // seek to position set by audio task (track deck CV scrubbing)
        if (uxBits & BIT_VOICE0_SEEK)
        {
            xSemaphoreTake(counter_mutex[0], portMAX_DELAY);
            b1 = 0;
            xSemaphoreGive(counter_mutex[0]);
            xSemaphoreTake(buffer_mutex[0], portMAX_DELAY);
            voice[0].voice_buffer = &audio_buffers[0];
            voice[0].voice_buffer->rpos = 0;
            xSemaphoreGive(buffer_mutex[0]);
            // audio_files[0].fpos already set by audio task; fill_audio_buffer uses it via f_lseek
            fill_audio_buffer(&voice[0].playback_engine, &audio_buffers[0], &audio_files[0], &file_mutex[0]);
            fill_audio_buffer(&voice[0].playback_engine, &audio_buffers[1], &audio_files[0], &file_mutex[0]);
        }
    }
}

static void file_reader_task_2(void *pvParams)
{
    uint32_t uxBits;
    ui_ev_ts_t ev;

    for (;;)
    {
        // wait for need to switch buffers
        xTaskNotifyWait(0x00,           /* Don't clear any notification bits on entry. */
                        ULONG_MAX,      /* Reset the notification value to 0 on exit. */
                        &uxBits,        /* Notified value pass out in
                                              ulNotifiedValue. */
                        portMAX_DELAY); /* Block indefinitely. */

        // switch audio buffers
        if (uxBits & BIT_VOICE1)
        {
            xSemaphoreTake(buffer_mutex[1], portMAX_DELAY);
            xSemaphoreTake(counter_mutex[1], portMAX_DELAY);
            voice[1].voice_buffer = &audio_buffers[(b2++ % 2) + 4];
            int buf_num = ((b2) % 2) + 4;
            xSemaphoreGive(counter_mutex[1]);  
            xSemaphoreGive(buffer_mutex[1]);    
            ev.event = EV_UPDATE_V1_POS;
            ev.event_data = (void*) (audio_files[1].fpos / (audio_files[1].fsize / 300)); // 300 is from bar width in visualization
            xQueueSend(ui_ev_queue, &ev, 0); 
            fill_audio_buffer(&voice[1].playback_engine, &audio_buffers[buf_num], &audio_files[1], &file_mutex[1]); 
        }

        if (uxBits & BIT_VOICE1_RETRIG)
        {
            xSemaphoreTake(counter_mutex[1], portMAX_DELAY);
            b2 = 0;
            xSemaphoreGive(counter_mutex[1]);
            xSemaphoreTake(buffer_mutex[1], portMAX_DELAY);
            voice[1].voice_buffer = &audio_buffers[3];
            voice[1].voice_buffer->rpos = 0;
            xSemaphoreGive(buffer_mutex[1]);
            jump_to_start(&voice[1].playback_engine, &audio_buffers[3], &audio_buffers[4], &audio_files[1], &file_mutex[1]);
        }

        // seek to position set by audio task (track deck CV scrubbing)
        if (uxBits & BIT_VOICE1_SEEK)
        {
            xSemaphoreTake(counter_mutex[1], portMAX_DELAY);
            b2 = 0;
            xSemaphoreGive(counter_mutex[1]);
            xSemaphoreTake(buffer_mutex[1], portMAX_DELAY);
            voice[1].voice_buffer = &audio_buffers[3];
            voice[1].voice_buffer->rpos = 0;
            xSemaphoreGive(buffer_mutex[1]);
            fill_audio_buffer(&voice[1].playback_engine, &audio_buffers[3], &audio_files[1], &file_mutex[1]);
            fill_audio_buffer(&voice[1].playback_engine, &audio_buffers[4], &audio_files[1], &file_mutex[1]);
        }
    }
}

static void file_manipulation_task(void *pvParams)
{
    uint32_t uxBits;

    for (;;)
    {

        xTaskNotifyWait(0x00,           /* Don't clear any notification bits on entry. */
                        ULONG_MAX,      /* Reset the notification value to 0 on exit. */
                        &uxBits,        /* Notified value pass out in
                                              ulNotifiedValue. */
                        portMAX_DELAY); /* Block indefinitely. */

        if (uxBits & BIT_VOICE_0_TRIG)
        {
            refill_first_buf(&voice[0].playback_engine, &audio_buffers[0], &audio_files[0], &file_mutex[0]);
        }

        if (uxBits & BIT_VOICE_1_TRIG)
        {
            refill_first_buf(&voice[1].playback_engine, &audio_buffers[3], &audio_files[1], &file_mutex[1]);
        }
    }
}

static void grain_loader_task(void *pvParams) {
    uint32_t uxBits;
    for (;;) {
        xTaskNotifyWait(0, ULONG_MAX, &uxBits, portMAX_DELAY);
        if (uxBits & BIT_GRAIN0_LOAD)
            granular_load_buffer(&gran[0], &audio_files[0], &file_mutex[0]);
        if (uxBits & BIT_GRAIN1_LOAD)
            granular_load_buffer(&gran[1], &audio_files[1], &file_mutex[1]);
    }
}

static inline void resetBufferToStart(int vid, TaskHandle_t *file_reader_task_handle)
{
    if (voice[vid].playback_engine.mode == PIPO && voice[vid].playback_engine.is_playback_direction_forward)
    {
        voice[vid].playback_engine.is_pipo_playback_forward = true;
    }
    else if (voice[vid].playback_engine.mode == PIPO && !voice[vid].playback_engine.is_playback_direction_forward)
    {
        voice[vid].playback_engine.is_pipo_playback_forward = false;
    }

    fifo_drop_samples(&voice_fifos[vid], voice_fifos[vid].size - voice_fifos[vid].free_slots);
    xTaskNotify(*file_reader_task_handle, 1 << (vid + 3), eSetBits);
}

static void pitch_init(int vid)
{
    voice[vid].playback_engine.phase = 1.0f;
    voice[vid].playback_engine.last_cummulated_sample = 0;
}

static inline void fill_FiFo(int vid, TaskHandle_t *file_reader_task_handle)
{

    fifo_t *fifo = &voice_fifos[vid];
    int samples_to_write = fifo->free_slots; // evaluate how many samples need to be written to FiFo

    xSemaphoreTake(buffer_mutex[vid], portMAX_DELAY);
    audio_b_t *voice_buf = voice[vid].voice_buffer;

    int voice_buf_len = (voice_buf->len / 4); // how many int32_t samples are available?

    if (voice_buf_len >= samples_to_write) // check if enough samples are available in buffer
    {
        fifo_write(fifo, (int32_t *)(voice_buf->buf + voice_buf->rpos), samples_to_write); // write samples to FiFo
        voice_buf->len -= (samples_to_write * 4);                                          // len stored in bytes
        voice_buf->rpos += (samples_to_write * 4);                                         // rpos stored in bytes
    }
    else // not enough samples in buffer, need to switch to new buffer
    {
        audio_b_t *temp_buf = NULL;
        int num_samples_old_buf = voice_buf_len;                                              // how many samples need to be read from old buffer
        fifo_write(fifo, (int32_t *)(voice_buf->buf + voice_buf->rpos), num_samples_old_buf); // read remaining samples from old buffer to FiFo
        samples_to_write -= voice_buf_len;                                                    // how many samples need to be read from new buffer
        voice_buf->len -= (num_samples_old_buf * 4);                                          // len stored in bytes
        voice_buf->rpos += (num_samples_old_buf * 4);                                         // rpos stored in bytes
        xSemaphoreTake(counter_mutex[vid], portMAX_DELAY);
        if (vid) // voice 2
        {
            temp_buf = &audio_buffers[((b2) % 2) + 4]; //switch temp to next buffer
        }
        else // voice 1
        {
            temp_buf = &audio_buffers[((b1) % 2) + 1]; // switch temp to next buffer
        }
        xSemaphoreGive(counter_mutex[vid]);
        fifo_write(fifo, (int32_t *)temp_buf->buf, samples_to_write); // read last samples from next buffer to fill FiFo
        temp_buf->len -= (samples_to_write * 4);
        temp_buf->rpos += (samples_to_write * 4);
    }

    xSemaphoreGive(buffer_mutex[vid]);

    if (voice_buf->len <= 0) // check if we need to switch buffer pointers for next run
        xTaskNotify(*file_reader_task_handle, 1 << vid, eSetBits);
}

// CV2 and CV3 have ±5V range via inverting op-amps; invert their raw ADC
// value so positive voltage produces positive modulation in all functions.
static const bool cv_bipolar[8] = {false, false, true, true, false, false, false, false};
static float env_follow = 0.f;
static uint16_t env_follow_cv = 0;
static inline uint16_t cv_corrected(int src, const uint16_t *ctrl_data) {
    if (src == 8) return env_follow_cv;
    if (src >= 0 && src < 8 && cv_bipolar[src])
        return (uint16_t)(4095u - ctrl_data[src]);
    return ctrl_data[src];
}

static float calculate_keyboard_pitch(uint16_t ctrl_data)
{
    if (ctrl_data <= 1426)
        return pitch_lut_24[0];
    if (ctrl_data >= 2599)
        return pitch_lut_24[24];
    return pitch_lut_24[(ctrl_data - 1424) / 49];
}

static float modulate_pan(int vid, int index, uint16_t *ctrl_data)
{
    float amount = 0.0;
    float input_value = 0.0;
    int src = -1;
    float ui_pan = ui_params[vid].pan;

    for (size_t i = 0; i < 9; i++)
    {
        if (matrix[i].dst == index)
        {
            amount = matrix[i].amt;
            if (amount == 0.0)
                return ui_pan;
            src = i;
            break;
        }
    }

    if (src >= 0 && src < 9)
    {
        input_value = FixedToFloat_8((cv_corrected(src, ctrl_data) >> 3)) - 1.0;
        input_value += ui_pan;
        if (input_value >= amount)
        {
            return amount;
        }
        if (input_value <= (-1.0 * amount))
        {
            return (-1.0 * amount);
        }
        return input_value;
    }

    return ui_pan;
}

static float modulate_pitch(int vid, int index, uint16_t *ctrl_data)
{
    float amount = 0.0;
    float input_value = 0.0;
    int src = -1;
    float ui_speed = ui_params[vid].playback_speed;

    for (size_t i = 0; i < 9; i++)
    {
        if (matrix[i].dst == index)
        {
            amount = matrix[i].amt;
            if (amount == 0.0){
                
                if(ui_speed >= 0){
                    voice[vid].playback_engine.is_playback_direction_forward = 1;
                }
                else{
                    voice[vid].playback_engine.is_playback_direction_forward = 0;
                }
                
                return ui_speed;
            }
                
            src = i;
            break;
        }
    }

    if (src >= 0 && src < 9)
    {
        input_value = FixedToFloat_8((cv_corrected(src, ctrl_data) >> 3)) - 1.0;
        if (input_value < 0.0 && voice[vid].playback_engine.is_playback_direction_forward)
        {
            voice[vid].playback_engine.is_playback_direction_forward = 0;
            if (voice[vid].playback_engine.mode == PIPO)
            {
                voice[vid].playback_engine.is_pipo_playback_forward = !voice[vid].playback_engine.is_pipo_playback_forward;
            }
            trigger_buffer_reload(vid, file_manipulation_task_handle);
        }
        else if (input_value >= 0.0 && !voice[vid].playback_engine.is_playback_direction_forward)
        {
            voice[vid].playback_engine.is_playback_direction_forward = 1;
            voice[vid].playback_engine.is_pipo_playback_forward = !voice[vid].playback_engine.is_pipo_playback_forward;
            trigger_buffer_reload(vid, file_manipulation_task_handle);
        }

        input_value = fabs(input_value);
        if (input_value >= amount)
        {
            return amount;
        }
        return input_value;
    }

    return 1.0;
}

static float modulatePitchParams(int vid, uint16_t *ctrl_data)
{
    float pitch_increment = 0.0;

    switch (vid)
    {
    case 0:
        pitch_increment = modulate_pitch(vid, MTX_V0_PB_SPEED, ctrl_data);
        break;
    case 1:
        pitch_increment = modulate_pitch(vid, MTX_V1_PB_SPEED, ctrl_data);
        break;
    }
    return pitch_increment;
}

static inline void interpolate_buffer(int vid, float *sample_left, float *sample_right, uint16_t *ctrl_data)
{
    float left_sample, right_sample, left_sample_next, right_sample_next;
    const uint32_t bit_mask_left = 0xFFFF, bit_mask_right = 0xFFFF0000;
    int32_t cummulated_sample, cummulated_sample_next;
    cummulated_sample = voice[vid].playback_engine.last_cummulated_sample;
    const float div = (1.0f / 32768.0f);
    float pitch_increment = 0.0;

    if (!ui_params[vid].is_pitch_cv_active)
        pitch_increment = voice[vid].playback_engine.pitch_increment;
    if (ui_params[vid].is_pitch_cv_active)
        pitch_increment = calculate_keyboard_pitch(ctrl_data[vid]) * matrix[vid].amt;
    pitch_increment *= modulatePitchParams(vid, ctrl_data);
    int32_t pos = (int32_t)(voice[vid].playback_engine.phase);
    float alpha = (float)(voice[vid].playback_engine.phase - pos);
    float inv_alpha = 1.0f - alpha;

    switch (pos)
    {
    case 2:                                      // one sample has to be skipped
        fifo_drop_samples(&voice_fifos[vid], 1); // drop next sample
        break;
    }

    if (pos)
    {
        fifo_pop_element(&voice_fifos[vid], &cummulated_sample); // read pos from FiFo and pop it from FiFo
        voice[vid].playback_engine.phase = alpha;                // we are just interested when the phase gets to next whole number
    }

    left_sample = div * ((int16_t)(bit_mask_left & cummulated_sample));           // parse left channel from data
    right_sample = div * ((int16_t)((bit_mask_right & cummulated_sample) >> 16)); // parse right channel from data

    fifo_read_element(&voice_fifos[vid], &cummulated_sample_next); // read pos+1 from FiFo for interpolation, but element resides in FiFo

    left_sample_next = div * ((int16_t)(bit_mask_left & cummulated_sample_next));           // parse pos+1 left channel from data
    right_sample_next = div * ((int16_t)((bit_mask_right & cummulated_sample_next) >> 16)); // parse pos+1 right channel from data

    left_sample = left_sample * inv_alpha + left_sample_next * alpha;    // interpolate next sample for left channel
    right_sample = right_sample * inv_alpha + right_sample_next * alpha; // interpolate next sample for right channel

    *sample_left = left_sample;
    *sample_right = right_sample;

    voice[vid].playback_engine.phase += pitch_increment; // increment phase position
    voice[vid].playback_engine.last_cummulated_sample = cummulated_sample;
}

static void buffer_add_float(float *out, float *in, int32_t len)
{
    for (int i = 0; i < len; i++)
    {
        out[i] += in[i];
    }
}

static void float_to_int(int32_t *out, float *in, int32_t len)
{
    const float mul = (8388608.0f);
    float sample;

    for (int i = 0; i < len; i++)
    {
        sample = in[i];
        sample *= mul;
        out[i] = hard_limit(round_float_int(sample)) << 8;
    }
}

static void parse_matrix_params(xQueueHandle event_queue, matrix_event_t *m_ev)
{
    //Fetch matrix events
    if (xQueueReceive(m_event_queue, m_ev, 0))
    {
        // ESP_LOGI("AUDIO", "Received matrix_event with Source: %d, Dst: %d, Amt: %d", m_ev->source, m_ev->changed_param, m_ev->amount);
        matrix[m_ev->source].dst = m_ev->changed_param;
        matrix[m_ev->source].amt = (float)m_ev->amount / 100.0;
    }
}

float modulateParameter(int index, uint16_t *ctrl_data)
{
    float amount = 0.0;
    float input_value = 0.0;
    int src = -1;

    for (size_t i = 0; i < 9; i++)
    {
        if (matrix[i].dst == index)
        {
            amount = matrix[i].amt;
            if (amount == 0.0)
                return 1.0;
            src = i;
            break;
        }
    }

    if (src >= 0 && src < 9)
    {

        input_value = FixedToFloat_9((cv_corrected(src, ctrl_data) >> 3));

        if (input_value >= amount)
        {
            return amount;
        }

        return input_value;
    }
    return 1.0;
}

static void modulateFilterParameter(int vid, int base, int width, int q_index, uint16_t *ctrl_data)
{
    float amount = 0.0;
    float input_value = 0.0;
    int src = -1;
    float cutoff_lp = 18000.0;
    float cutoff_hp = 20.0;
    int index_base = 0, index_width = 0;
    int ui_base_id = ui_params[vid].filter_base_index;
    float base_frq = poti_vals[ui_base_id];
    int ui_width_id = ui_params[vid].filter_width_index;
    float width_frq = poti_vals[ui_width_id];
    float ui_resonance = ui_params[vid].resonance;
    float resonance = 0.0;

    // Parse Base Modulation
    for (size_t i = 0; i < 9; i++)
    {
        if (matrix[i].dst == base)
        {
            amount = matrix[i].amt;
            if (amount == 0.0)
                base_frq = poti_vals[ui_base_id];
            src = i;
            break;
        }
    }

    if (src >= 0 && src < 9)
    {
        int index = 0;
        int array_amt = amount * 511;
        input_value = FixedToFloat_8((cv_corrected(src, ctrl_data) >> 3)) - 1.0;
        input_value *= 255;
        if (input_value + ui_base_id <= -1 * (ui_base_id + array_amt))
        {
            index = ui_base_id + array_amt;
        }
        else if (input_value + ui_base_id >= (ui_base_id + array_amt))
        {
            index = (ui_base_id + array_amt);
        }
        else
        {
            index = input_value + ui_base_id;
        }

        if (index <= 0)
            index = 0;
        if (index > 511)
            index = 511;

        index_base = index;
    }
    else
    {
        index_base = ui_base_id;
    }

    // Parse Width Modulation
    src = -1;

    for (size_t i = 0; i < 9; i++)
    {
        if (matrix[i].dst == width)
        {
            amount = matrix[i].amt;
            if (amount == 0.0)
                width_frq = poti_vals[ui_width_id];
            src = i;
            break;
        }
    }

    if (src >= 0 && src < 9)
    {
        int index = 0;
        int array_amt = amount * 511;
        input_value = FixedToFloat_9((cv_corrected(src, ctrl_data) >> 3));
        input_value *= 511;

        if (input_value + ui_width_id >= array_amt)
        {
            index = array_amt;
        }
        else
        {
            index = input_value + ui_width_id;
        }

        index_width = index;
    }
    else
    {
        index_width = ui_width_id;
    }

    cutoff_lp = poti_vals[index_base] + poti_vals[index_width] / 2;
    cutoff_hp = poti_vals[index_base] - poti_vals[index_width] / 2;

    if (cutoff_hp > 18000)
    {
        cutoff_hp = 18000;
    }
    if (cutoff_lp < 20)
    {
        cutoff_lp = 20;
    }
    if (cutoff_lp > 18000)
    {
        cutoff_lp = 18000;
    }
    if (cutoff_hp < 20)
    {
        cutoff_hp = 20;
    }

    src = -1;
    for (size_t i = 0; i < 9; i++)
    {
        if (matrix[i].dst == q_index)
        {
            amount = matrix[i].amt;
            if (amount == 0.0)
                resonance = ui_resonance;
            src = i;
            break;
        }
    }

    if (src >= 0 && src < 9)
    {

        input_value = FixedToFloat_9((cv_corrected(src, ctrl_data) >> 3));
        input_value *= 5.0f;
        resonance = ui_resonance + input_value;
        amount *= 5.0f;

        if (resonance >= amount)
        {
            return;
        }
    }
    else
        resonance = ui_resonance;

    set_filter_params(voice[vid].highpass_filter, cutoff_hp, resonance);
    set_filter_params(voice[vid].lowpass_filter, cutoff_lp, resonance);
}

static float modulateDlySendParameter(int vid, int index, uint16_t *ctrl_data)
{
    float ui_send = ui_params[vid].delay_send;
    float amount = 0.0;
    float input_value = 0.0;
    int src = -1;

    for (size_t i = 0; i < 9; i++)
    {
        if (matrix[i].dst == index)
        {
            amount = matrix[i].amt;
            if (amount == 0.0)
                return ui_send;
            src = i;
            break;
        }
    }

    if (src >= 0 && src < 9)
    {

        input_value = FixedToFloat_9((ctrl_data[src] >> 3));

        if (input_value >= amount)
        {
            return amount;
        }

        return input_value;
    }

    return ui_send;
}

static void modulate_adsr_parameter(int vid, uint16_t* ctrl_data)
{
    bool time_value_modulated = false;
    
    voice[vid].adsr.attack_time = ui_params[vid].attack_time * 44.1f;
    voice[vid].adsr.decay_time = ui_params[vid].decay_time * 44.1f;
    voice[vid].adsr.sustain_level = ui_params[vid].sustain_level * 0.01f;
    voice[vid].adsr.release_time = ui_params[vid].release_time * 44.1f;

    for(size_t i = 2; i < 9; i++)
    {
        if((matrix[i].dst == MTX_V0_ADSR_ATTACK && vid == 0) || 
           (matrix[i].dst == MTX_V1_ADSR_ATTACK && vid == 1))
        {
            voice[vid].adsr.attack_time = modulate_unipolar(ui_params[vid].attack_time, 10000, FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt) * 44.1f;
            time_value_modulated = true;
        }

        if((matrix[i].dst == MTX_V0_ADSR_DECAY && vid == 0) || 
           (matrix[i].dst == MTX_V1_ADSR_DECAY && vid == 1))
        {
            voice[vid].adsr.decay_time = modulate_unipolar(ui_params[vid].decay_time, 10000, FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt) * 44.1f;
            time_value_modulated = true;
        }

        if((matrix[i].dst == MTX_V0_ADSR_SUSTAIN && vid == 0) || 
           (matrix[i].dst == MTX_V1_ADSR_SUSTAIN && vid == 1))
        {
            voice[vid].adsr.sustain_level = modulate_unipolar(ui_params[vid].sustain_level, 100, FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt) * 0.01f;
        }

        if((matrix[i].dst == MTX_V0_ADSR_RELEASE && vid == 0) || 
           (matrix[i].dst == MTX_V1_ADSR_RELEASE && vid == 1))
        {
            voice[vid].adsr.release_time = modulate_unipolar(ui_params[vid].release_time, 10000, FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt) * 44.1f;
            time_value_modulated = true;
        }

        if(time_value_modulated)
        {
            calculate_adsr_phase_increment(&voice[vid].adsr);
        }

        time_value_modulated = false;
    }
    
}

static uint32_t last_seek_pos[2] = {0, 0};

static void modulatePlaymodeParameters(int vid, uint16_t *ctrl_data)
{
    uint32_t *sample_start = &voice[vid].playback_engine.sample_start;
    uint32_t *loop_start = &voice[vid].playback_engine.loop_start;
    uint32_t *loop_end = &voice[vid].playback_engine.loop_end;
    uint32_t temp_sample_start = voice[vid].playback_engine.sample_start,
             temp_loop_start = voice[vid].playback_engine.loop_start,
             temp_loop_end = voice[vid].playback_engine.loop_end;
    uint32_t fsize = audio_files[vid].fsize;
    float temp_grain_pos     = (float)ui_params[vid].grain_position;
    float temp_grain_spray   = (float)ui_params[vid].grain_spray;
    float temp_grain_size    = (float)ui_params[vid].grain_size_ms;
    float temp_grain_density = (float)ui_params[vid].grain_density;
    float temp_grain_pitch   = (float)ui_params[vid].grain_pitch;
    float temp_grain_pspray  = (float)ui_params[vid].grain_pitch_spray;

    for (size_t i = 2; i < 9; i++)
    {
        if ((matrix[i].dst == MTX_V0_POSITION && vid == 0) ||
            (matrix[i].dst == MTX_V1_POSITION && vid == 1))
        {
            float cv_val = FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3));
            uint32_t target = (uint32_t)(cv_val * (float)fsize);
            target = (target / 4) * 4;
            if (target < temp_sample_start) target = temp_sample_start;
            if (target + 4 > temp_loop_end) target = temp_loop_end > 4 ? temp_loop_end - 4 : 0;
            uint32_t threshold = fsize / 200;
            if (threshold < 4) threshold = 4;
            uint32_t delta = target > last_seek_pos[vid] ? target - last_seek_pos[vid] : last_seek_pos[vid] - target;
            if (delta >= threshold) {
                last_seek_pos[vid] = target;
                audio_files[vid].fpos = target;
                fifo_drop_samples(&voice_fifos[vid], voice_fifos[vid].size - voice_fifos[vid].free_slots);
                voice[vid].playback_engine.phase = 1.0f;
                xTaskNotify(vid == 0 ? file_reader_task_1_handle : file_reader_task_2_handle,
                            vid == 0 ? BIT_VOICE0_SEEK : BIT_VOICE1_SEEK, eSetBits);
            }
        }
        else if ((matrix[i].dst == MTX_V0_MODE_START && vid == 0) ||
                 (matrix[i].dst == MTX_V1_MODE_START && vid == 1))
        {
            temp_sample_start = (uint32_t)modulate_unipolar(ui_params[vid].sample_start, fsize, FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt);
            temp_sample_start /= 4;
            temp_sample_start *= 4;
        }
        else if ((matrix[i].dst == MTX_V0_MODE_LSTART && vid == 0) ||
                 (matrix[i].dst == MTX_V1_MODE_LSTART && vid == 1))
        {
            temp_loop_start = (uint32_t)modulate_unipolar(ui_params[vid].loop_start, fsize, FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt);
            temp_loop_start /= 4;
            temp_loop_start *= 4;
        }
        else if ((matrix[i].dst == MTX_V0_MODE_LEND && vid == 0) ||
                 (matrix[i].dst == MTX_V1_MODE_LEND && vid == 1))
        {
            temp_loop_end = (uint32_t)modulate_unipolar(ui_params[vid].loop_end, 0, 1.0f - FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt);
            temp_loop_end /= 4;
            temp_loop_end *= 4;
        }
        else if ((matrix[i].dst == MTX_V0_MODE_LWIDTH && vid == 0) ||
                 (matrix[i].dst == MTX_V1_MODE_LWIDTH && vid == 1))
        {
            float cv_val = FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3));
            uint32_t center = (ui_params[vid].loop_start + ui_params[vid].loop_end) / 2;
            uint32_t hw = (uint32_t)(cv_val * matrix[i].amt * (float)fsize * 0.5f);
            hw = (hw / 4) * 4;
            temp_loop_start = center > hw ? center - hw : 0;
            temp_loop_end = center + hw < fsize ? center + hw : fsize;
            temp_loop_start = (temp_loop_start / 4) * 4;
            temp_loop_end = (temp_loop_end / 4) * 4;
        }
        else if ((matrix[i].dst == MTX_V0_GRAIN_POS && vid == 0) ||
                 (matrix[i].dst == MTX_V1_GRAIN_POS && vid == 1))
        {
            float cv_val = FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3));
            temp_grain_pos = modulate_unipolar(temp_grain_pos, 100.0f, cv_val, matrix[i].amt);
            if (temp_grain_pos < 0.0f) temp_grain_pos = 0.0f;
            if (temp_grain_pos > 100.0f) temp_grain_pos = 100.0f;
        }
        else if ((matrix[i].dst == MTX_V0_GRAIN_SPRAY && vid == 0) ||
                 (matrix[i].dst == MTX_V1_GRAIN_SPRAY && vid == 1))
        {
            float cv_val = FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3));
            temp_grain_spray = modulate_unipolar(temp_grain_spray, 100.0f, cv_val, matrix[i].amt);
            if (temp_grain_spray < 0.0f) temp_grain_spray = 0.0f;
            if (temp_grain_spray > 100.0f) temp_grain_spray = 100.0f;
        }
        else if ((matrix[i].dst == MTX_V0_GRAIN_SIZE && vid == 0) ||
                 (matrix[i].dst == MTX_V1_GRAIN_SIZE && vid == 1))
        {
            float cv_val = FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3));
            temp_grain_size = modulate_unipolar(temp_grain_size, 500.0f, cv_val, matrix[i].amt);
            if (temp_grain_size < 5.0f) temp_grain_size = 5.0f;
            if (temp_grain_size > 500.0f) temp_grain_size = 500.0f;
        }
        else if ((matrix[i].dst == MTX_V0_GRAIN_DENSITY && vid == 0) ||
                 (matrix[i].dst == MTX_V1_GRAIN_DENSITY && vid == 1))
        {
            float cv_val = FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3));
            temp_grain_density = modulate_unipolar(temp_grain_density, 50.0f, cv_val, matrix[i].amt);
            if (temp_grain_density < 1.0f) temp_grain_density = 1.0f;
            if (temp_grain_density > 50.0f) temp_grain_density = 50.0f;
        }
        else if ((matrix[i].dst == MTX_V0_GRAIN_PITCH && vid == 0) ||
                 (matrix[i].dst == MTX_V1_GRAIN_PITCH && vid == 1))
        {
            float cv_val = FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3));
            temp_grain_pitch = modulate_unipolar(temp_grain_pitch, 24.0f, cv_val, matrix[i].amt);
            if (temp_grain_pitch < -24.0f) temp_grain_pitch = -24.0f;
            if (temp_grain_pitch > 24.0f) temp_grain_pitch = 24.0f;
        }

        if (temp_loop_start >= temp_loop_end)
        {
            if (temp_loop_end == 0)
            {
                temp_loop_end = 4;
                temp_loop_start = 0;
            }
            else
            {
                temp_loop_start = temp_loop_end - 4;
            }
        }

        if (temp_sample_start >= temp_loop_end)
        {
            if (temp_loop_end == 0)
            {
                temp_loop_end = 4;
                temp_sample_start = 0;
            }
            else
            {
                temp_sample_start = temp_loop_end - 4;
            }
        }

        voice[vid].playback_engine.sample_start = temp_sample_start;

        voice[vid].playback_engine.loop_start = temp_loop_start;

        voice[vid].playback_engine.loop_end = temp_loop_end;
    }

    if (voice[vid].playback_engine.mode == GRAIN) {
        granular_update_params(&gran[vid],
            temp_grain_pos / 100.0f,
            temp_grain_spray / 100.0f,
            temp_grain_size,
            temp_grain_density,
            temp_grain_pitch,
            temp_grain_pspray / 100.0f);
        if (granular_load_needed(&gran[vid], fsize))
            xTaskNotify(grain_loader_task_handle,
                vid == 0 ? BIT_GRAIN0_LOAD : BIT_GRAIN1_LOAD, eSetBits);
    }
}

static void modulateDlyParameters(delay_t *delay, delay_cfg_t cfg, uint16_t *ctrlData)
{
    bool isModulated = false;
    for (size_t i = 2; i < 9; i++) // first two are for pitch cv
    {
        switch (matrix[i].dst)
        {
        case MTX_DELAY_TIME:
            isModulated = true;
            cfg.msLength += ((float)cv_corrected(i, ctrlData) - 2048.0) * 0.00048828125f * matrix[i].amt * DELAY_MAX_LENGTH_MS;
            if (cfg.msLength < 2.0)
            {
                cfg.msLength = 2.0;
                break;
            }
            if (cfg.msLength > DELAY_MAX_LENGTH_MS)
                cfg.msLength = DELAY_MAX_LENGTH_MS;
            break;
        case MTX_DELAY_PAN:
            isModulated = true;
            cfg.pan += ((float)cv_corrected(i, ctrlData) - 2048.0) * 0.00048828125f * matrix[i].amt;
            if (cfg.pan < 0.0)
            {
                cfg.pan = 0.0;
                break;
            }
            if (cfg.pan > 1.0)
                cfg.pan = 1.0;
            break;
        case MTX_DELAY_FB:
            isModulated = true;
            cfg.feedback += ((float)cv_corrected(i, ctrlData) - 2048.0) * 0.00048828125f * matrix[i].amt;
            if (cfg.feedback < 0.0)
            {
                cfg.feedback = 0.0;
                break;
            }
            if (cfg.feedback > 1.1)
                cfg.feedback = 1.1;
            break;
        case MTX_DELAY_VOL:
            isModulated = true;
            cfg.volume += ((float)cv_corrected(i, ctrlData) - 2048.0) * 0.00048828125f * matrix[i].amt;
            if (cfg.volume < 0.0)
            {
                cfg.volume = 0.0;
                break;
            }
            if (cfg.volume > 1.1)
                cfg.volume = 1.1;
            break;
        default:
            break;
        }
    }
    /* Debug values
    ESP_LOGE("DELAY_MOD", "matrix %f, Time %.2f, FB %.2f, Pan %.2f, vol %.2f", matrix[4].amt, cfg.msLength, cfg.feedback, cfg.pan, cfg.volume);
    */
    if (isModulated)
    {
        delay_set_params_smooth(delay, cfg);
    }
}

static void process_next_audio_block(float *buffer, float *dly_send, int32_t buffer_len, int8_t vid, QueueHandle_t param_queue, uint16_t *ctrl_data, param_data_t *params, TaskHandle_t *file_reader_task_handle)
{
    float left_sample, right_sample;

    if (voice[vid].lowpass_filter->is_active && vid == 0)
    {
        modulateFilterParameter(vid, MTX_V0_FILTER_BASE, MTX_V0_FILTER_WIDTH, MTX_V0_FILTER_Q, ctrl_data);
    }

    if (voice[vid].lowpass_filter->is_active && vid == 1)
    {
        modulateFilterParameter(vid, MTX_V1_FILTER_BASE, MTX_V0_FILTER_WIDTH, MTX_V1_FILTER_Q, ctrl_data);
    }

    modulate_adsr_parameter(vid, ctrl_data);

    float modulateDlySend[2];
    modulateDlySend[0] = modulateDlySendParameter(vid, MTX_V0_DLY_SEND, ctrl_data);
    modulateDlySend[1] = modulateDlySendParameter(vid, MTX_V1_DLY_SEND, ctrl_data);
    float modulateVolume[2];
    modulateVolume[0] = modulateParameter(MTX_V0_VOLUME, ctrl_data);
    modulateVolume[1] = modulateParameter(MTX_V1_VOLUME, ctrl_data);
    float modulatePan[2];
    modulatePan[0] = modulate_pan(0, MTX_V0_PAN, ctrl_data);
    modulatePan[1] = modulate_pan(1, MTX_V1_PAN, ctrl_data);
    float modulateDist[2];
    modulateDist[0] = modulateParameter(MTX_V0_DIST_DRIVE, ctrl_data);
    modulateDist[1] = modulateParameter(MTX_V1_DIST_DRIVE, ctrl_data);

    bool grain_mode = (voice[vid].playback_engine.mode == GRAIN);

    for (int sample = 0; sample < buffer_len / 2; sample++)
    {
        if (grain_mode)
            granular_process_sample(&gran[vid], &left_sample, &right_sample);
        else
            interpolate_buffer(vid, &left_sample, &right_sample, ctrl_data);
        get_next_eg_val(&voice[vid].adsr, &left_sample, &right_sample);  // calculate envelope value for sample

        left_sample *= ui_params[vid].volume * modulateVolume[vid];
        right_sample *= ui_params[vid].volume * modulateVolume[vid];

        if (ui_params[vid].is_shaper_active)
        { // apply tanh shaper
            left_sample = fasttanh(ui_params[vid].dist_amp * modulateDist[vid] * left_sample);
            right_sample = fasttanh(ui_params[vid].dist_amp * modulateDist[vid] * right_sample);
        }

        if (voice[vid].lowpass_filter->is_active) // apply lowpass filter
            filter_sound(voice[vid].lowpass_filter, &left_sample, &right_sample);
        if (voice[vid].highpass_filter->is_active) // apply highpass filter
            filter_sound(voice[vid].highpass_filter, &left_sample, &right_sample);

        // more consistent semantic needed here, compare with previous modulations
        stereo_balance(modulatePan[vid], &left_sample, &right_sample);

        // todo same semantics as before, i.e. return default ui param in modDlySend
        dly_send[2 * sample] += left_sample * ui_params[vid].delay_send * modulateDlySend[vid];
        dly_send[2 * sample + 1] += right_sample * ui_params[vid].delay_send * modulateDlySend[vid];

        buffer[2 * sample] += left_sample * fade(&voice[vid].fade[0]);
        buffer[2 * sample + 1] += right_sample * fade(&voice[vid].fade[1]);

        if (voice[vid].fade[0].state == OFF)
        {
            if (!grain_mode)
                resetBufferToStart(vid, file_reader_task_handle);

            pitch_init(vid);
            reset_filter(voice[vid].highpass_filter);
            reset_filter(voice[vid].lowpass_filter);
            update_eg_state(&voice[vid].adsr, 1);
            voice[vid].fade[0].state = LISTEN;
            voice[vid].fade[1].state = LISTEN;
        }
    }
}

static int check_state(int vid)
{

    if (voice[vid].adsr.adsr_state == OFF)
        return true;
    return false;
}

void enableTrigModeLatch(uint8_t vid){
    trigModeLatch[vid] = true;
}

void disableTrigModeLatch(uint8_t vid){
    trigModeLatch[vid] = false;
}

static void audio_task(void *pvParams)
{
    // audio buffers
    int32_t out[BUF_SZ], in[BUF_SZ];
    float float_sum[BUF_SZ], fx_buf[BUF_SZ], dly_send[BUF_SZ];
    size_t nb;
    // control data (9 = 8 hardware CVs + env follower at index 8)
    uint16_t ctrlData[9];
    // matrix data
    matrix_event_t m_ev;
    // create file interaction tasks
    xTaskCreate(file_reader_task_1, "file_reader_task", 4096, NULL, 22, &file_reader_task_1_handle);
    xTaskCreate(file_reader_task_2, "file_reader_task", 4096, NULL, 22, &file_reader_task_2_handle);
    xTaskCreate(file_manipulation_task, "file_manipulation_task", 4096, NULL, 22, &file_manipulation_task_handle);
    xTaskCreate(grain_loader_task, "grain_loader", 4096, NULL, 18, &grain_loader_task_handle);
    // mode control data
    play_state_data_t play_state_data;
    param_data_t param_data[2];
    // effects data
    effect_data_t effectData;
    effectData.delay.is_active = 0;
    // delay data
    delay_t delay;
    delay_cfg_t delayCfg;
    delayCfg.msLength = 250.0;
    delayCfg.feedback = 0.25;
    delayCfg.pan = 0.5;
    delayCfg.volume = 1.0;
    delayCfg.mode = DELAY_STEREO;
    delay_create(&delay, 44100.0, DELAY_MAX_LENGTH_MS, delayCfg);
    // external in data
    ext_in_cfg_t extInCfg;
    extInCfg.volume = 0.0f;
    extInCfg.pan = 0.0f;
    extInCfg.delay_send = 0.0f;

    uint8_t trigPreVal[2], trigVal[2];
    const uint8_t trigPin[2] = {TRIG0_PIN, TRIG1_PIN};
    trigPreVal[0] = trigVal[0] = gpio_get_level(trigPin[0]);
    trigPreVal[1] = trigVal[1] = gpio_get_level(trigPin[1]);

    for (;;)
    {
        // get control data
        xQueueReceive(control_queue, &ctrlData, 0);

        // always read I2S ADC: compute envelope follower + feed ext-in/recording
        i2s_read(I2S_NUM_0, in, BUF_SZ * 4, &nb, portMAX_DELAY);
        {
            float env_in = 0.f;
            for (int _i = 0; _i < BUF_SZ; _i++) {
                float s = (float)in[_i] * DIVISOR;
                env_in += s * s;
            }
            env_in = sqrtf(env_in / BUF_SZ);
            env_follow = env_follow * 0.9f + env_in * 0.1f;
            env_follow_cv = (uint16_t)(env_follow * 4095.f);
        }
        ctrlData[8] = env_follow_cv;

        if (recording_is_active())
            recording_push(in);

        // update shared status for REST /status endpoint (non-critical, best effort)
        portENTER_CRITICAL(&_status_mux);
        memcpy(_audio_status.cv, ctrlData, sizeof(ctrlData));
        strncpy(_audio_status.v0, audio_files[0].fname, 31); _audio_status.v0[31] = 0;
        strncpy(_audio_status.v1, audio_files[1].fname, 31); _audio_status.v1[31] = 0;
        portEXIT_CRITICAL(&_status_mux);

        // init buf
        memset(out, 0, 64 * 2);
        memset(fx_buf, 0, sizeof(float) * BUF_SZ);

        if (effectData.extInData.is_active)
        {
            float bl = extInCfg.pan < 0.0f ? 1.0f : 1.0f - extInCfg.pan;
            float br = extInCfg.pan < 0.0f ? 1.0f + extInCfg.pan : 1.0f;
            //ESP_LOGE("EXT", "bl %.2f, br %.2f, pan %.2f", bl, br, extInCfg.pan);
            bl *= extInCfg.volume * DIVISOR;
            br *= extInCfg.volume * DIVISOR;
            for (int i = 0; i < BUF_SZ / 2; i++)
            {
                float_sum[i * 2] = (float)in[i * 2] * bl;
                float_sum[i * 2 + 1] = (float)in[i * 2 + 1] * br;
                dly_send[i * 2] = float_sum[i * 2] * extInCfg.delay_send;
                dly_send[i * 2 + 1] = float_sum[i * 2 + 1] * extInCfg.delay_send;
            }
        }
        else
        {
            memset(float_sum, 0, sizeof(float) * BUF_SZ);
            memset(dly_send, 0, sizeof(float) * BUF_SZ);
        }

        // check for trigs -> polling gpios
        for (int vid = 0; vid < 2; vid++){
            static audio_ev_t eventPreVal[2] = {EV_TRG_NONE, EV_TRG_NONE};
            audio_ev_t event = EV_NONE;
            // poll trig button 
            trigVal[vid] = gpio_get_level(trigPin[vid]);
            // derive event state from trig buttons
            if(trigVal[vid] != trigPreVal[vid]){
                trigPreVal[vid] = trigVal[vid];
                if(trigModeLatch[vid]){
                    if(trigVal[vid] == 0){
                        if(eventPreVal[vid] == EV_TRG_DOWN) event = EV_TRG_UP;
                        else event = EV_TRG_DOWN;
                        eventPreVal[vid] = event;
                    }
                }else{
                    if(trigVal[vid] == 0){
                        event = EV_TRG_DOWN;
                    } 
                    else{
                        event = EV_TRG_UP;
                    }
                    eventPreVal[vid] = event;
                }
                //ESP_LOGI("AUDIO", "Trigger happened vid %d event %d", vid, event);
            }

            // Route trigger based on configured function
            trig_func_t trig_fn = recording_get_trig_func(vid);
            if (trig_fn == TRIG_FUNC_RECORD || trig_fn == TRIG_FUNC_TRANSPORT) {
                if (event == EV_TRG_DOWN) recording_start();
                else if (event == EV_TRG_UP) recording_stop();
                if (trig_fn == TRIG_FUNC_RECORD) continue; // voice suspended
                // TRANSPORT falls through to also trigger the voice
            }

            // action
            if(event == EV_TRG_DOWN)
            {
                if (voice[vid].adsr.adsr_state == OFF)
                {
                    if (voice[vid].playback_engine.mode != GRAIN) {
                        if (vid)
                            resetBufferToStart(vid, &file_reader_task_2_handle);
                        else
                            resetBufferToStart(vid, &file_reader_task_1_handle);
                    }

                    pitch_init(vid);
                    reset_filter(voice[vid].highpass_filter);
                    reset_filter(voice[vid].lowpass_filter);
                    update_eg_state(&voice[vid].adsr, event);
                }
                else
                {
                    process_fade_state(&voice[vid], 1);
                }
            }
            else
            {
                update_eg_state(&voice[vid].adsr, event);
            }
        }

        parse_play_state_data(&mode_handle_v1, &play_state_data, &ui_params[1], &file_manipulation_task_handle, &voice[1], &audio_files[1], play_modes, 1);
        parse_play_state_data(&mode_handle_v0, &play_state_data, &ui_params[0], &file_manipulation_task_handle, &voice[0], &audio_files[0], play_modes, 0);
        parse_voice_param_data(&ui_parameter_queue_v1, &voice[1], &ui_params[1], &param_data[1]);
        parse_voice_param_data(&ui_parameter_queue_v0, &voice[0], &ui_params[0], &param_data[0]);
        parse_play_direction(&playbackspeed_state_queue_v1, &file_manipulation_task_handle, &voice[1], 1);
        parse_play_direction(&playbackspeed_state_queue_v0, &file_manipulation_task_handle, &voice[0], 0);
        

        //Fetch and store matrix events/data
        parse_matrix_params(m_event_queue, &m_ev);

        //Modulate sample start, loop start, loop end, grain params
        modulatePlaymodeParameters(1, ctrlData);
        modulatePlaymodeParameters(0, ctrlData);

        // do the DSP
        for (int i = 0; i < 2; i++)
        {
            if (check_state(i))
            {
                continue;
            }

            if (i)
            {
                if (voice[i].playback_engine.mode != GRAIN)
                    fill_FiFo(i, &file_reader_task_2_handle);
                process_next_audio_block(float_sum, dly_send, BUF_SZ, i, ui_parameter_queue_v1, ctrlData, &param_data[1], &file_reader_task_2_handle);
            }

            else
            {
                if (voice[i].playback_engine.mode != GRAIN)
                    fill_FiFo(i, &file_reader_task_1_handle);
                process_next_audio_block(float_sum, dly_send, BUF_SZ, i, ui_parameter_queue_v0, ctrlData, &param_data[0], &file_reader_task_1_handle);
            }
        }

        // update effect parameters
        if (xQueueReceive(effect_parameter_queue, &effectData, 0))
        {
            // delay
            //ESP_LOGE("DELAY", "time %.2f, feedback %.2f, pan %.2f, vol %.2f, mode %d", delayCfg.msLength, delayCfg.feedback, delayCfg.pan, delayCfg.volume, delayCfg.mode);
            delayCfg.msLength = (float)effectData.delay.time;
            delayCfg.feedback = effectData.delay.feedback * 0.01;
            delayCfg.pan = FixedToFloat_7(effectData.delay.pan);
            delayCfg.volume = effectData.delay.volume * 0.01;
            delayCfg.mode = effectData.delay.mode;
            delay_set_params(&delay, delayCfg);
            // external in
            extInCfg.volume = effectData.extInData.volume * 0.01;
            extInCfg.pan = FixedToFloat_14(effectData.extInData.pan);
            extInCfg.delay_send = effectData.extInData.delay_send * 0.01;
            //ESP_LOGE("EXTIN", "volume %f, pan %f, delay send %f", extInCfg.volume, extInCfg.pan, extInCfg.delay_send);
        }

        // process delay modulation from matrix
        modulateDlyParameters(&delay, delayCfg, ctrlData);

        // process delay
        if (effectData.delay.is_active)
        {
            delay_process(&delay, dly_send, float_sum, BUF_SZ);
        }

        // send of to dac
        float_to_int(out, float_sum, BUF_SZ);

        i2s_write(I2S_NUM_0, out, BUF_SZ * 4, &nb, portMAX_DELAY);
    }
}

void assignAudioFiles()
{
    //ESP_LOGI("AUDIO", "Assigning slot id audio files");
    cJSON *cfgData = readJSONFileAsCJSON("/sdcard/CONFIG.JSN");
    if(cfgData != NULL){
        cJSON *slots = cJSON_GetObjectItem(cfgData, "slots");
        if(slots != NULL){
            cJSON *fileObj;
            FRESULT fr;
            // critical section re-assigning audio file slots
            for (int i = 0; i < 2; i++)
            {
                fileObj = cJSON_GetArrayItem(slots, i);
                //ESP_LOGE("FILE", "File %s", cJSON_GetObjectItemCaseSensitive(fileObj, "file")->valuestring);
                char *filename = cJSON_GetObjectItemCaseSensitive(fileObj, "file")->valuestring;
                if(strcmp(filename, "") != 0){
                    xSemaphoreTake(file_mutex[i], portMAX_DELAY);
                    if (strcmp(audio_files[i].fname, filename) != 0) // new file to be assigned
                    {
                        float ui_sample_start = 0.0f, ui_loop_start = 0.0f, ui_loop_end = 1.0f;

                        // calculate percentage ui values
                        if(audio_files[i].fsize > 0){
                            ui_sample_start = voice[i].playback_engine.sample_start / (float) audio_files[i].fsize;
                            ui_loop_start = voice[i].playback_engine.loop_start / (float) audio_files[i].fsize;
                            ui_loop_end = voice[i].playback_engine.loop_end / (float) audio_files[i].fsize;
                        }
                        


                        // close file 
                        f_close(&audio_files[i].fil);

                        

                        // open file 
                        strcpy(audio_files[i].fname, filename);
                        fr = f_open(&audio_files[i].fil, audio_files[i].fname, FA_READ);
                        if (fr)
                        {
                            ESP_LOGE("AUDIO", "Error opening file %s", audio_files[i].fname);
                            repairAudioFileAssigment(i);
                            abort();
                        }

                        // enable fastseek on file
                        audio_files[i].fil.cltbl = audio_files[i].clmt;
                        audio_files[i].fil.cltbl[0] = SZ_TBL;
                        fr = f_lseek(&audio_files[i].fil, CREATE_LINKMAP);
                        if (fr)
                            ESP_LOGE("AUDIO", "Creating linkmap table for fatfs fileseek not successful!");

                        // set correct initial read positions
                        audio_files[i].fsize = (uint32_t)f_size(&audio_files[i].fil);
                        //ESP_LOGI("AUDIO", "Voice %d Filesize %d", i, audio_files[i].fsize);
                        voice[i].playback_engine.loop_end = audio_files[i].fsize;
                        if (voice[i].playback_engine.is_playback_direction_forward)
                        {
                            audio_files[i].fpos = 0;
                            f_lseek(&audio_files[i].fil, 0);
                        }
                        else
                        {
                            audio_files[i].fpos = voice[i].playback_engine.loop_end;
                            f_lseek(&audio_files[i].fil, audio_files[i].fsize);
                        }
                        
                        fill_audio_buffer(&voice[i].playback_engine, &audio_buffers[i * 3], &audio_files[i], NULL);
                        //calculate new values for sample start, loop start, loop end
                        uint32_t sample_start = (ui_sample_start * audio_files[i].fsize / 4);
                        sample_start *= 4;
                        voice[i].playback_engine.sample_start = sample_start;

                        uint32_t loop_start = (ui_loop_start * audio_files[i].fsize / 4);
                        loop_start *= 4;
                        voice[i].playback_engine.loop_start = loop_start;

                        uint32_t loop_end = (ui_loop_end * audio_files[i].fsize / 4);
                        loop_end *= 4;
                        voice[i].playback_engine.loop_end = loop_end;

                    }
                    xSemaphoreGive(file_mutex[i]);
                }else ESP_LOGE("AUDIO", "Couldn't assign audio file due to slot being empty");
            }
            // end critical section
        }else ESP_LOGE("AUDIO", "Couldn't fetch slots from config");
    }else ESP_LOGE("AUDIO", "Couldn't open config.jsn");
    cJSON_Delete(cfgData);
}

static void initAudioStructs()
{
    // init audio file structures
    bzero(audio_files, sizeof(audio_f_t) * 2);
    bzero(audio_buffers, sizeof(audio_b_t) * 6);
    bzero(voice, sizeof(voice_t) * 2);
    bzero(voice_fifos, sizeof(fifo_t) * 2);
    for (int i = 0; i < 6; i++)
    {
        audio_buffers[i].buf = heap_caps_calloc(SD_BUF_SZ, 1, MALLOC_CAP_INTERNAL);
        if (audio_buffers[i].buf == NULL)
        {
            ESP_LOGE("AUDIO", "Failed allocating audio buffer %d", i);
        }
    }

    //Init matrix - every source/amount/destination to 0
    for (int j = 0; j < 9; j++)
    {
        if (j != 0 && j != 1)
        {
            matrix[j].dst = MTX_NONE;
        }
        else
        {
            if (j == 0)
                matrix[j].dst = MTX_V0_PITCH;
            if (j == 1)
                matrix[j].dst = MTX_V1_PITCH;
        }
        matrix[j].amt = 0.0f;
    }

    for (int i = 0; i < 2; i++)
    {
        voice[i].voice_buffer = &audio_buffers[i * 3];
        init_adsr(&voice[i].adsr);
        init_ui_params(&ui_params[i]);
        voice[i].lowpass_filter = (Filter_t *)heap_caps_malloc(sizeof(Filter_t), MALLOC_CAP_INTERNAL);
        voice[i].highpass_filter = (Filter_t *)heap_caps_malloc(sizeof(Filter_t), MALLOC_CAP_INTERNAL);
        init_play_mode(&voice[i].playback_engine);
        init_filter(voice[i].lowpass_filter, lowpass);
        init_filter(voice[i].highpass_filter, highpass);
        init_fade(&voice[i]);
        fifo_init(&voice_fifos[i], (int32_t *)heap_caps_calloc(BUF_SZ + 1, sizeof(int32_t), MALLOC_CAP_INTERNAL), BUF_SZ + 1);
        granular_init(&gran[i]);
    }

    bzero(controlData, sizeof(unsigned int) * 8);
}

void initAudio(xQueueHandle ui_queue_v0, xQueueHandle ui_queue_v1, xQueueHandle eff_queue, 
    xQueueHandle cv_queue_v0, xQueueHandle cv_queue_v1, xQueueHandle _mode_handle_v0, xQueueHandle _mode_handle_v1, 
    xQueueHandle matrix_event_queue, xQueueHandle ui_ev_q)
{
    // create and start audio task
    ui_parameter_queue_v0 = ui_queue_v0;        //get ui parameter queueHandle
    ui_parameter_queue_v1 = ui_queue_v1;        //get ui parameter queueHandle
    effect_parameter_queue = eff_queue;         //get ui effect parameter queueHandle
    playbackspeed_state_queue_v0 = cv_queue_v0; //get pitch cv active parameter queueHandle
    playbackspeed_state_queue_v1 = cv_queue_v1;
    mode_handle_v0 = _mode_handle_v0;
    mode_handle_v1 = _mode_handle_v1; //get mod matrix from ui thread
    m_event_queue = matrix_event_queue;
    ui_ev_queue = ui_ev_q;
    ESP_LOGI("AUDIO", "Starting audio task");

    for (int i = 0; i < 2; i++)
    {
        file_mutex[i] = xSemaphoreCreateMutex();
        if (!file_mutex[i])
            ESP_LOGE("AUDIO", "Couldn't create mutex for audio file struct");
    }
    for (int i = 0; i < 2; i++)
    {
        buffer_mutex[i] = xSemaphoreCreateMutex();
        if (!buffer_mutex[i])
            ESP_LOGE("AUDIO", "Couldn't create mutex for audio buffer struct");
    }
    for (int i = 0; i < 2; i++)
    {
        counter_mutex[i] = xSemaphoreCreateMutex();
        if (!counter_mutex[i])
            ESP_LOGE("AUDIO", "Couldn't create mutex for buffer position counter");
    }

    control_queue = xQueueCreate(10, sizeof(uint16_t) * 8);
    initSpiPer(control_queue);
    init_i2s();

    recording_init();
    initAudioStructs();
    assignAudioFiles();

    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 23, &audio_task_h, 1);

}
