// s2_machine_sampler — the original two-voice Strämpler engine as a machine.
// Split out of components/audio in M0b; menus follow in M0c.
#include "audio.h"
#include "machine_sampler2.h"
#include "ui.h"
#include "preset.h"
#include <sys/stat.h>
#include "dsp_lib.h"
#include "fill_buffer.h"
#include "audio_luts.h"
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
#include "adsr.h"
#include "modulation.h"
#include "recording.h"
#include "machine.h"

#define DIR_FWD 0
#define DIR_BWD 1
#define BIT_VOICE0 0x01
#define BIT_VOICE1 0x02
#define BIT_VOICE_0_TRIG 0x06
#define BIT_VOICE_1_TRIG 0xC
#define BIT_VOICE0_RETRIG 0x08
#define BIT_VOICE1_RETRIG 0x10
#define BIT_CONTROL_DATA 0x04
#define SAMPLE_RATE 44100
#define FRACTION_MASK 0x1ff
#define DELAY_MAX_LENGTH_MS 1500.0
// Using Q16 Unsigned Fixed Point Notation for ENV_LUT
#define LUT_SCALE 16
#define MUL_ENV(x, y) (((int)(x) * (y)) >> LUT_SCALE)
#define FixedToDouble2(x) ((double)(x) / (1 << 9))
#define DIVISOR 0.0000000004656612873077392578125f // is 1.0f/2^31

// machine-switch teardown: sampler_stop() raises the flag, wakes the SD
// tasks, and joins them via the counting semaphore before freeing anything
static volatile bool s_shutdown = false;
static SemaphoreHandle_t s_task_done = NULL;

static audio_f_t audio_files[2];
static audio_b_t audio_buffers[6];
static voice_t voice[2];
static fifo_t voice_fifos[2];
static matrix_row_t matrix[8];
// Sampler2: keyboard pitch is no longer hard-wired to matrix rows 0/1 — any
// row may carry MTX_Vx_PITCH. Cache the row per voice; -1 = unassigned.
static int s_pitch_row[2] = {0, 1};
static ui_param_holder_t ui_params[2];
static void (*play_modes[])(void *, void *, void *) = {s2_fill_buffer_one_shot, s2_fill_buffer_loop, s2_fill_buffer_pipo};

int s2_controlData[3][8];

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
static bool arm_monitor[2] = {false, false};

static SemaphoreHandle_t counter_mutex[2] = {NULL};
static SemaphoreHandle_t file_mutex[2] = {NULL};
static SemaphoreHandle_t buffer_mutex[2] = {NULL};
static uint32_t b1 = 0, b2 = 0;
TaskHandle_t s2_file_manipulation_task_handle = NULL;

// trig mode, usin stdatomic here as the variable may be changed from another thread / core
atomic_bool s2_trigModeLatch[2] = {false, false};

void s2_trigger_buffer_reload(int vid, TaskHandle_t *handle)
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

        if (s_shutdown) {               // sampler_stop() is waiting to join us
            xSemaphoreGive(s_task_done);
            vTaskDelete(NULL);
        }

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
            s2_fill_audio_buffer(&voice[0].playback_engine, &audio_buffers[buf_num], &audio_files[0], &file_mutex[0]);     
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
            s2_jump_to_start(&voice[0].playback_engine, &audio_buffers[0], &audio_buffers[1], &audio_files[0], &file_mutex[0]);
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

        if (s_shutdown) {               // sampler_stop() is waiting to join us
            xSemaphoreGive(s_task_done);
            vTaskDelete(NULL);
        }

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
            s2_fill_audio_buffer(&voice[1].playback_engine, &audio_buffers[buf_num], &audio_files[1], &file_mutex[1]); 
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
            s2_jump_to_start(&voice[1].playback_engine, &audio_buffers[3], &audio_buffers[4], &audio_files[1], &file_mutex[1]);
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

        if (s_shutdown) {               // sampler_stop() is waiting to join us
            xSemaphoreGive(s_task_done);
            vTaskDelete(NULL);
        }

        if (uxBits & BIT_VOICE_0_TRIG)
        {
            s2_refill_first_buf(&voice[0].playback_engine, &audio_buffers[0], &audio_files[0], &file_mutex[0]);
        }

        if (uxBits & BIT_VOICE_1_TRIG)
        {
            s2_refill_first_buf(&voice[1].playback_engine, &audio_buffers[3], &audio_files[1], &file_mutex[1]);
        }
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

    s2_fifo_drop_samples(&voice_fifos[vid], voice_fifos[vid].size - voice_fifos[vid].free_slots);
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
        s2_fifo_write(fifo, (int32_t *)(voice_buf->buf + voice_buf->rpos), samples_to_write); // write samples to FiFo
        voice_buf->len -= (samples_to_write * 4);                                          // len stored in bytes
        voice_buf->rpos += (samples_to_write * 4);                                         // rpos stored in bytes
    }
    else // not enough samples in buffer, need to switch to new buffer
    {
        audio_b_t *temp_buf = NULL;
        int num_samples_old_buf = voice_buf_len;                                              // how many samples need to be read from old buffer
        s2_fifo_write(fifo, (int32_t *)(voice_buf->buf + voice_buf->rpos), num_samples_old_buf); // read remaining samples from old buffer to FiFo
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
        s2_fifo_write(fifo, (int32_t *)temp_buf->buf, samples_to_write); // read last samples from next buffer to fill FiFo
        temp_buf->len -= (samples_to_write * 4);
        temp_buf->rpos += (samples_to_write * 4);
    }

    xSemaphoreGive(buffer_mutex[vid]);

    if (voice_buf->len <= 0) // check if we need to switch buffer pointers for next run
        xTaskNotify(*file_reader_task_handle, 1 << vid, eSetBits);
}

static float calculate_keyboard_pitch(uint16_t ctrl_data)
{
    if (ctrl_data <= 1426)
        return s2_pitch_lut_24[0];
    if (ctrl_data >= 2599)
        return s2_pitch_lut_24[24];
    return s2_pitch_lut_24[(ctrl_data - 1424) / 49];
}

static float modulate_pan(int vid, int index, uint16_t *ctrl_data)
{
    float amount = 0.0;
    float input_value = 0.0;
    int src = -1;
    float ui_pan = ui_params[vid].pan;

    for (size_t i = 0; i < 8; i++)
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

    if (src >= 0 && src < 8)
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

    for (size_t i = 0; i < 8; i++)
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

    if (src >= 0 && src < 8)
    {
        input_value = FixedToFloat_8((cv_corrected(src, ctrl_data) >> 3)) - 1.0;
        if (input_value < 0.0 && voice[vid].playback_engine.is_playback_direction_forward)
        {
            voice[vid].playback_engine.is_playback_direction_forward = 0;
            if (voice[vid].playback_engine.mode == PIPO)
            {
                voice[vid].playback_engine.is_pipo_playback_forward = !voice[vid].playback_engine.is_pipo_playback_forward;
            }
            s2_trigger_buffer_reload(vid, s2_file_manipulation_task_handle);
        }
        else if (input_value >= 0.0 && !voice[vid].playback_engine.is_playback_direction_forward)
        {
            voice[vid].playback_engine.is_playback_direction_forward = 1;
            voice[vid].playback_engine.is_pipo_playback_forward = !voice[vid].playback_engine.is_pipo_playback_forward;
            s2_trigger_buffer_reload(vid, s2_file_manipulation_task_handle);
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
    if (ui_params[vid].is_pitch_cv_active) {
        int r = s_pitch_row[vid];
        if (r >= 0)
            pitch_increment = calculate_keyboard_pitch(cv_corrected(r, ctrl_data)) * matrix[r].amt;
        else
            pitch_increment = voice[vid].playback_engine.pitch_increment;
    }
    pitch_increment *= modulatePitchParams(vid, ctrl_data);
    int32_t pos = (int32_t)(voice[vid].playback_engine.phase);
    float alpha = (float)(voice[vid].playback_engine.phase - pos);
    float inv_alpha = 1.0f - alpha;

    switch (pos)
    {
    case 2:                                      // one sample has to be skipped
        s2_fifo_drop_samples(&voice_fifos[vid], 1); // drop next sample
        break;
    }

    if (pos)
    {
        s2_fifo_pop_element(&voice_fifos[vid], &cummulated_sample); // read pos from FiFo and pop it from FiFo
        voice[vid].playback_engine.phase = alpha;                // we are just interested when the phase gets to next whole number
    }

    left_sample = div * ((int16_t)(bit_mask_left & cummulated_sample));           // parse left channel from data
    right_sample = div * ((int16_t)((bit_mask_right & cummulated_sample) >> 16)); // parse right channel from data

    s2_fifo_read_element(&voice_fifos[vid], &cummulated_sample_next); // read pos+1 from FiFo for interpolation, but element resides in FiFo

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
        out[i] = s2_hard_limit(s2_round_float_int(sample)) << 8;
    }
}

static void update_pitch_rows(void)
{
    s_pitch_row[0] = s_pitch_row[1] = -1;
    for (int i = 7; i >= 0; i--) {
        if (matrix[i].dst == MTX_V0_PITCH) s_pitch_row[0] = i;
        if (matrix[i].dst == MTX_V1_PITCH) s_pitch_row[1] = i;
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
        update_pitch_rows();
    }
}

float s2_modulateParameter(int index, uint16_t *ctrl_data)
{
    float amount = 0.0;
    float input_value = 0.0;
    int src = -1;

    for (size_t i = 0; i < 8; i++)
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

    if (src >= 0 && src < 8)
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
    float base_frq = s2_poti_vals[ui_base_id];
    int ui_width_id = ui_params[vid].filter_width_index;
    float width_frq = s2_poti_vals[ui_width_id];
    float ui_resonance = ui_params[vid].resonance;
    float resonance = 0.0;

    // Parse Base Modulation
    for (size_t i = 0; i < 8; i++)
    {
        if (matrix[i].dst == base)
        {
            amount = matrix[i].amt;
            if (amount == 0.0)
                base_frq = s2_poti_vals[ui_base_id];
            src = i;
            break;
        }
    }

    if (src >= 0 && src < 8)
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

    for (size_t i = 0; i < 8; i++)
    {
        if (matrix[i].dst == width)
        {
            amount = matrix[i].amt;
            if (amount == 0.0)
                width_frq = s2_poti_vals[ui_width_id];
            src = i;
            break;
        }
    }

    if (src >= 0 && src < 8)
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

    cutoff_lp = s2_poti_vals[index_base] + s2_poti_vals[index_width] / 2;
    cutoff_hp = s2_poti_vals[index_base] - s2_poti_vals[index_width] / 2;

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
    for (size_t i = 0; i < 8; i++)
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

    if (src >= 0 && src < 8)
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

    s2_set_filter_params(voice[vid].highpass_filter, cutoff_hp, resonance);
    s2_set_filter_params(voice[vid].lowpass_filter, cutoff_lp, resonance);
}

static float modulateDlySendParameter(int vid, int index, uint16_t *ctrl_data)
{
    float ui_send = ui_params[vid].delay_send;
    float amount = 0.0;
    float input_value = 0.0;
    int src = -1;

    for (size_t i = 0; i < 8; i++)
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

    if (src >= 0 && src < 8)
    {

        input_value = FixedToFloat_9((cv_corrected(src, ctrl_data) >> 3));

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

    if (!ui_params[vid].env_on) {
        // envelope bypassed: transparent gate with 1 ms anti-click edges;
        // dialed values and matrix ADSR modulation are ignored
        voice[vid].adsr.attack_time   = 44.1f;
        voice[vid].adsr.decay_time    = 44.1f;
        voice[vid].adsr.sustain_level = 1.0f;
        voice[vid].adsr.release_time  = 44.1f;
        s2_calculate_adsr_phase_increment(&voice[vid].adsr);
        return;
    }

    voice[vid].adsr.attack_time = ui_params[vid].attack_time * 44.1f;
    voice[vid].adsr.decay_time = ui_params[vid].decay_time * 44.1f;
    voice[vid].adsr.sustain_level = ui_params[vid].sustain_level * 0.01f;
    voice[vid].adsr.release_time = ui_params[vid].release_time * 44.1f;

    for(size_t i = 2; i < 8; i++)
    {
        if((matrix[i].dst == MTX_V0_ADSR_ATTACK && vid == 0) || 
           (matrix[i].dst == MTX_V1_ADSR_ATTACK && vid == 1))
        {
            voice[vid].adsr.attack_time = s2_modulate_unipolar(ui_params[vid].attack_time, 10000, FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt) * 44.1f;
            time_value_modulated = true;
        }

        if((matrix[i].dst == MTX_V0_ADSR_DECAY && vid == 0) || 
           (matrix[i].dst == MTX_V1_ADSR_DECAY && vid == 1))
        {
            voice[vid].adsr.decay_time = s2_modulate_unipolar(ui_params[vid].decay_time, 10000, FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt) * 44.1f;
            time_value_modulated = true;
        }

        if((matrix[i].dst == MTX_V0_ADSR_SUSTAIN && vid == 0) || 
           (matrix[i].dst == MTX_V1_ADSR_SUSTAIN && vid == 1))
        {
            voice[vid].adsr.sustain_level = s2_modulate_unipolar(ui_params[vid].sustain_level, 100, FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt) * 0.01f;
        }

        if((matrix[i].dst == MTX_V0_ADSR_RELEASE && vid == 0) || 
           (matrix[i].dst == MTX_V1_ADSR_RELEASE && vid == 1))
        {
            voice[vid].adsr.release_time = s2_modulate_unipolar(ui_params[vid].release_time, 10000, FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt) * 44.1f;
            time_value_modulated = true;
        }

        if(time_value_modulated)
        {
            s2_calculate_adsr_phase_increment(&voice[vid].adsr);
        }

        time_value_modulated = false;
    }
    
}

static void modulatePlaymodeParameters(int vid, uint16_t *ctrl_data)
{
    uint32_t *sample_start = &voice[vid].playback_engine.sample_start;
    uint32_t *loop_start = &voice[vid].playback_engine.loop_start;
    uint32_t *loop_end = &voice[vid].playback_engine.loop_end;
    uint32_t temp_sample_start = voice[vid].playback_engine.sample_start,
             temp_loop_start = voice[vid].playback_engine.loop_start,
             temp_loop_end = voice[vid].playback_engine.loop_end;
    uint32_t fsize = audio_files[vid].fsize;

    for (size_t i = 2; i < 8; i++)
    {
        if ((matrix[i].dst == MTX_V0_MODE_START && vid == 0) ||
            (matrix[i].dst == MTX_V1_MODE_START && vid == 1))
        {
            temp_sample_start = (uint32_t)s2_modulate_unipolar(ui_params[vid].sample_start, fsize, FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt);
            temp_sample_start /= 4;
            temp_sample_start *= 4;
        }
        else if ((matrix[i].dst == MTX_V0_MODE_LSTART && vid == 0) ||
                 (matrix[i].dst == MTX_V1_MODE_LSTART && vid == 1))
        {
            temp_loop_start = (uint32_t)s2_modulate_unipolar(ui_params[vid].loop_start, fsize, FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt);
            temp_loop_start /= 4;
            temp_loop_start *= 4;
        }
        else if ((matrix[i].dst == MTX_V0_MODE_LEND && vid == 0) ||
                 (matrix[i].dst == MTX_V1_MODE_LEND && vid == 1))
        {
            temp_loop_end = (uint32_t)s2_modulate_unipolar(ui_params[vid].loop_end, 0, 1.0f - FixedToFloat_9((cv_corrected(i, ctrl_data) >> 3)), matrix[i].amt);
            temp_loop_end /= 4;
            temp_loop_end *= 4;
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
}

static void modulateDlyParameters(delay_t *delay, delay_cfg_t cfg, uint16_t *ctrlData)
{
    bool isModulated = false;
    for (size_t i = 2; i < 8; i++) // first two are for pitch cv
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
        s2_delay_set_params_smooth(delay, cfg);
    }
}

static void update_arm_monitor(void)
{
    for (int vid = 0; vid < 2; vid++) {
        bool is_recording = recording_is_active() && (recording_get_target_vid() == vid);
        bool is_armed     = recording_get_trig_func(vid) == TRIG_FUNC_RECORD;
        if (is_recording)
            arm_monitor[vid] = true;
        else if (is_armed)
            arm_monitor[vid] = recording_get_arm_monitor() && !s2_isVoicePlaying(vid);
        else
            arm_monitor[vid] = false;
    }
}

static void processRecArmCV(uint16_t *ctrlData)
{
    static bool prev_arm[2] = {false, false};
    for (int i = 0; i < 8; i++) {
        int vid = -1;
        if (matrix[i].dst == MTX_V0_REC_ARM) vid = 0;
        else if (matrix[i].dst == MTX_V1_REC_ARM) vid = 1;
        if (vid < 0) continue;
        bool gate_high = cv_corrected(i, ctrlData) > 2048;
        if (gate_high == prev_arm[vid]) continue;
        prev_arm[vid] = gate_high;
        recording_set_trig_func(vid, gate_high ? TRIG_FUNC_RECORD : TRIG_FUNC_VOICE);
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
    modulateVolume[0] = s2_modulateParameter(MTX_V0_VOLUME, ctrl_data);
    modulateVolume[1] = s2_modulateParameter(MTX_V1_VOLUME, ctrl_data);
    float modulatePan[2];
    modulatePan[0] = modulate_pan(0, MTX_V0_PAN, ctrl_data);
    modulatePan[1] = modulate_pan(1, MTX_V1_PAN, ctrl_data);
    float modulateDist[2];
    modulateDist[0] = s2_modulateParameter(MTX_V0_DIST_DRIVE, ctrl_data);
    modulateDist[1] = s2_modulateParameter(MTX_V1_DIST_DRIVE, ctrl_data);

    for (int sample = 0; sample < buffer_len / 2; sample++)
    {
        interpolate_buffer(vid, &left_sample, &right_sample, ctrl_data); // get the next samples for processing
        s2_get_next_eg_val(&voice[vid].adsr, &left_sample, &right_sample);  // calculate envelope value for sample

        left_sample *= ui_params[vid].volume * modulateVolume[vid];
        right_sample *= ui_params[vid].volume * modulateVolume[vid];

        if (ui_params[vid].is_shaper_active)
        { // apply tanh shaper
            left_sample = s2_fasttanh(ui_params[vid].dist_amp * modulateDist[vid] * left_sample);
            right_sample = s2_fasttanh(ui_params[vid].dist_amp * modulateDist[vid] * right_sample);
        }

        if (voice[vid].lowpass_filter->is_active) // apply lowpass filter
            s2_filter_sound(voice[vid].lowpass_filter, &left_sample, &right_sample);
        if (voice[vid].highpass_filter->is_active) // apply highpass filter
            s2_filter_sound(voice[vid].highpass_filter, &left_sample, &right_sample);

        // more consistent semantic needed here, compare with previous modulations
        s2_stereo_balance(modulatePan[vid], &left_sample, &right_sample);

        // todo same semantics as before, i.e. return default ui param in modDlySend
        dly_send[2 * sample] += left_sample * ui_params[vid].delay_send * modulateDlySend[vid];
        dly_send[2 * sample + 1] += right_sample * ui_params[vid].delay_send * modulateDlySend[vid];

        buffer[2 * sample] += left_sample * s2_fade(&voice[vid].s2_fade[0]);
        buffer[2 * sample + 1] += right_sample * s2_fade(&voice[vid].s2_fade[1]);

        if (voice[vid].s2_fade[0].state == OFF)
        {
            resetBufferToStart(vid, file_reader_task_handle); // Reset Buffer Pointer to Start

            pitch_init(vid);
            s2_reset_filter(voice[vid].highpass_filter);
            s2_reset_filter(voice[vid].lowpass_filter);
            s2_update_eg_state(&voice[vid].adsr, 1);
            voice[vid].s2_fade[0].state = LISTEN;
            voice[vid].s2_fade[1].state = LISTEN;
        }
    }
}

static int check_state(int vid)
{

    if (voice[vid].adsr.adsr_state == OFF)
        return true;
    return false;
}

void s2_enableTrigModeLatch(uint8_t vid){
    s2_trigModeLatch[vid] = true;
}

void s2_disableTrigModeLatch(uint8_t vid){
    s2_trigModeLatch[vid] = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// s2_machine_sampler — the original two-voice sampler behind the machine
// interface. M0a: engine still lives in this file; M0b moves it out.
// State below was audio_task()'s stack before the split.
// ─────────────────────────────────────────────────────────────────────────────

static float float_sum[BUF_SZ], fx_buf[BUF_SZ], dly_send[BUF_SZ];
static play_state_data_t play_state_data;
static param_data_t param_data[2];
static effect_data_t effectData;
static delay_t delay;
static delay_cfg_t delayCfg;
static ext_in_cfg_t extInCfg;
static uint8_t trigPreVal[2], trigVal[2];
static const uint8_t trigPin[2] = {TRIG0_PIN, TRIG1_PIN};

static void sampler_process(int32_t out[MACHINE_BLOCK], const int32_t in[MACHINE_BLOCK], const machine_io_t *io)
{
    // legacy: this engine applies cv_corrected() itself at each call site
    const uint16_t *ctrlData = io->cv_raw;
    matrix_event_t m_ev;

        // init buf
        memset(out, 0, 64 * 2);
        memset(fx_buf, 0, sizeof(float) * BUF_SZ);

        bool auto_mon = recording_is_active() || arm_monitor[0] || arm_monitor[1];
        if (effectData.extInData.is_active || auto_mon)
        {
            float mon_vol = (auto_mon && extInCfg.volume == 0.0f) ? 1.0f : extInCfg.volume;
            float bl = extInCfg.pan < 0.0f ? 1.0f : 1.0f - extInCfg.pan;
            float br = extInCfg.pan < 0.0f ? 1.0f + extInCfg.pan : 1.0f;
            bl *= mon_vol * DIVISOR;
            br *= mon_vol * DIVISOR;
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
                if(s2_trigModeLatch[vid]){
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

            // if this trigger is routed to recording, toggle on each TRG_DOWN and skip voice
            if (recording_get_trig_func(vid) == TRIG_FUNC_RECORD) {
                if (event == EV_TRG_DOWN) {
                    if (recording_is_active()) {
                        recording_stop();
                        // auto-switch back to voice; file will load when writer finishes
                        recording_set_trig_func(vid, TRIG_FUNC_VOICE);
                    } else {
                        recording_start(vid);
                    }
                }
                continue;
            }

            // action
            if(event == EV_TRG_DOWN)
            {
                if (voice[vid].adsr.adsr_state == OFF)
                {
                    if (vid)
                        resetBufferToStart(vid, &file_reader_task_2_handle); // Reset Buffer Pointer to Start
                    else
                        resetBufferToStart(vid, &file_reader_task_1_handle); // Reset Buffer Pointer to Start

                    pitch_init(vid);
                    s2_reset_filter(voice[vid].highpass_filter);
                    s2_reset_filter(voice[vid].lowpass_filter);
                    s2_update_eg_state(&voice[vid].adsr, event);
                }
                else
                {
                    s2_process_fade_state(&voice[vid], 1);
                }
            }
            else
            {
                s2_update_eg_state(&voice[vid].adsr, event);
            }
        }

        // auto-load completed recording into the voice slot
        {
            int load_vid; char load_fname[48];
            if (recording_poll_load(&load_vid, load_fname)) {
                ESP_LOGI("REC", "Auto-loading %s into voice %d", load_fname, load_vid);
                cJSON *cfg = readJSONFileAsCJSON("/sdcard/CONFIG.JSN");
                if (cfg) {
                    cJSON *slots = cJSON_GetObjectItem(cfg, "slots");
                    cJSON *slot = slots ? cJSON_GetArrayItem(slots, load_vid) : NULL;
                    if (slot) {
                        // derive FatFS path: strip "/sdcard" VFS prefix
                        const char *fatfs_path = load_fname;
                        if (strncmp(load_fname, "/sdcard", 7) == 0) fatfs_path = load_fname + 7;
                        // derive display name: basename without .RAW
                        const char *slash = strrchr(fatfs_path, '/');
                        const char *base = slash ? slash + 1 : fatfs_path;
                        char id[48];
                        int blen = strlen(base);
                        if (blen >= 4 && strcasecmp(base + blen - 4, ".RAW") == 0) blen -= 4;
                        snprintf(id, sizeof(id), "%.*s", blen, base);
                        cJSON_ReplaceItemInObjectCaseSensitive(slot, "name", cJSON_CreateString(id));
                        cJSON_ReplaceItemInObjectCaseSensitive(slot, "file", cJSON_CreateString(fatfs_path));
                        char *cfg_str = cJSON_Print(cfg);
                        if (cfg_str) { writeJSONFile("/sdcard/CONFIG.JSN", cfg_str); free(cfg_str); }
                    }
                    cJSON_Delete(cfg);
                }
                s2_assignAudioFiles();
            }
        }

        s2_parse_play_state_data(&mode_handle_v1, &play_state_data, &ui_params[1], &s2_file_manipulation_task_handle, &voice[1], &audio_files[1], play_modes, 1);
        s2_parse_play_state_data(&mode_handle_v0, &play_state_data, &ui_params[0], &s2_file_manipulation_task_handle, &voice[0], &audio_files[0], play_modes, 0);
        s2_parse_voice_param_data(&ui_parameter_queue_v1, &voice[1], &ui_params[1], &param_data[1]);
        s2_parse_voice_param_data(&ui_parameter_queue_v0, &voice[0], &ui_params[0], &param_data[0]);
        s2_parse_play_direction(&playbackspeed_state_queue_v1, &s2_file_manipulation_task_handle, &voice[1], 1);
        s2_parse_play_direction(&playbackspeed_state_queue_v0, &s2_file_manipulation_task_handle, &voice[0], 0);
        

        //Fetch and store matrix events/data
        parse_matrix_params(m_event_queue, &m_ev);

        //Modulate sample start, loop start, loop end
        modulatePlaymodeParameters(1, ctrlData);
        modulatePlaymodeParameters(0, ctrlData);

        // do the DSP
        for (int i = 0; i < 2; i++)
        {
            if (check_state(i))
            {
                continue;
            }

            bool mute_voice = recording_is_active() && (recording_get_target_vid() == i);

            if (i)
            {
                fill_FiFo(i, &file_reader_task_2_handle);
                if (!mute_voice)
                    process_next_audio_block(float_sum, dly_send, BUF_SZ, i, ui_parameter_queue_v1, ctrlData, &param_data[1], &file_reader_task_2_handle);
            }

            else
            {
                fill_FiFo(i, &file_reader_task_1_handle);
                if (!mute_voice)
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
            s2_delay_set_params(&delay, delayCfg);
            // external in
            extInCfg.volume = effectData.extInData.volume * 0.01;
            extInCfg.pan = FixedToFloat_14(effectData.extInData.pan);
            extInCfg.delay_send = effectData.extInData.delay_send * 0.01;
            //ESP_LOGE("EXTIN", "volume %f, pan %f, delay send %f", extInCfg.volume, extInCfg.pan, extInCfg.delay_send);
        }

        // process delay modulation from matrix
        modulateDlyParameters(&delay, delayCfg, ctrlData);
        processRecArmCV(ctrlData);
        update_arm_monitor();

        // process delay
        if (effectData.delay.is_active)
        {
            s2_delay_process(&delay, dly_send, float_sum, BUF_SZ);
        }

        // send of to dac
        float_to_int(out, float_sum, BUF_SZ);
}

static esp_err_t sampler_start(void)
{
    s_shutdown = false;
    s_task_done = xSemaphoreCreateCounting(3, 0);

    // the machine owns its parameter queues (menu side binds to the same ones)
    xQueueHandle q_v0   = xQueueCreate(1, sizeof(param_data_t));
    xQueueHandle q_v1   = xQueueCreate(1, sizeof(param_data_t));
    xQueueHandle q_eff  = xQueueCreate(1, sizeof(effect_data_t));
    xQueueHandle q_pbs0 = xQueueCreate(1, sizeof(bool));
    xQueueHandle q_pbs1 = xQueueCreate(1, sizeof(bool));
    xQueueHandle q_md0  = xQueueCreate(1, sizeof(play_state_data_t));
    xQueueHandle q_md1  = xQueueCreate(1, sizeof(play_state_data_t));
    xQueueHandle q_mtx  = xQueueCreate(10, sizeof(matrix_event_t));
    xQueueHandle q_ev   = uiGetEventQueue();
    s2_sampler_bind_queues(q_v0, q_v1, q_eff, q_pbs0, q_pbs1, q_md0, q_md1, q_mtx, q_ev);
    s2_sampler_menu_bind_queues(q_v0, q_v1, q_eff, q_pbs0, q_pbs1, q_md0, q_md1, q_mtx, q_ev);

    // the sampler bootstraps its own SD artifacts (was core fileio's job)
    struct stat st;
    if (stat("/sdcard/banks/default.JSN", &st) == -1)
        s2_initBank("/sdcard/banks/default.JSN");

    s2_sampler_boot_init();   // mutexes + audio structs + slot file assignment

    xTaskCreate(file_reader_task_1, "file_reader_task", 4096, NULL, 22, &file_reader_task_1_handle);
    xTaskCreate(file_reader_task_2, "file_reader_task", 4096, NULL, 22, &file_reader_task_2_handle);
    xTaskCreate(file_manipulation_task, "file_manipulation_task", 4096, NULL, 22, &s2_file_manipulation_task_handle);

    effectData.delay.is_active = 0;
    delayCfg.msLength = 250.0;
    delayCfg.feedback = 0.25;
    delayCfg.pan = 0.5;
    delayCfg.volume = 1.0;
    delayCfg.mode = DELAY_STEREO;
    s2_delay_create(&delay, 44100.0, DELAY_MAX_LENGTH_MS, delayCfg);

    extInCfg.volume = 0.0f;
    extInCfg.pan = 0.0f;
    extInCfg.delay_send = 0.0f;

    trigPreVal[0] = trigVal[0] = gpio_get_level(trigPin[0]);
    trigPreVal[1] = trigVal[1] = gpio_get_level(trigPin[1]);
    return ESP_OK;
}

static void sampler_stop(void)
{
    // machine_core has already detached us from the audio task and let the
    // in-flight block drain, so process() cannot be running past this point.

    // join the three SD tasks: raise the flag, wake them out of their
    // notify-waits, and collect one "done" tick from each before freeing
    // anything they might still be touching
    s_shutdown = true;
    xTaskNotify(file_reader_task_1_handle, 0, eNoAction);
    xTaskNotify(file_reader_task_2_handle, 0, eNoAction);
    xTaskNotify(s2_file_manipulation_task_handle, 0, eNoAction);
    for (int i = 0; i < 3; i++)
        if (xSemaphoreTake(s_task_done, pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE("SAMPLER", "stop(): SD task %d did not exit", i);
    vSemaphoreDelete(s_task_done);
    s_task_done = NULL;

    for (int i = 0; i < 2; i++)
        f_close(&audio_files[i].fil);   // harmless FR_INVALID_OBJECT if closed

    for (int i = 0; i < 6; i++) {
        free(audio_buffers[i].buf);
        audio_buffers[i].buf = NULL;
    }

    for (int i = 0; i < 2; i++) {
        vSemaphoreDelete(file_mutex[i]);    file_mutex[i] = NULL;
        vSemaphoreDelete(buffer_mutex[i]);  buffer_mutex[i] = NULL;
        vSemaphoreDelete(counter_mutex[i]); counter_mutex[i] = NULL;
    }

    s2_delay_free(&delay);

    // the parameter queues are ours (created in start); unhook the menu side
    // first — its pages are unregistered by the menusys rebuild, so nothing
    // sends on them past this point
    s2_sampler_menu_bind_queues(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    vQueueDelete(ui_parameter_queue_v0);        ui_parameter_queue_v0 = NULL;
    vQueueDelete(ui_parameter_queue_v1);        ui_parameter_queue_v1 = NULL;
    vQueueDelete(effect_parameter_queue);       effect_parameter_queue = NULL;
    vQueueDelete(playbackspeed_state_queue_v0); playbackspeed_state_queue_v0 = NULL;
    vQueueDelete(playbackspeed_state_queue_v1); playbackspeed_state_queue_v1 = NULL;
    vQueueDelete(mode_handle_v0);               mode_handle_v0 = NULL;
    vQueueDelete(mode_handle_v1);               mode_handle_v1 = NULL;
    vQueueDelete(m_event_queue);                m_event_queue = NULL;
    ui_ev_queue = NULL;                          // core's queue, not ours to delete

    audio_status_set_voices("", "");
}

// M0c-1: the sampler's menu UI still compiles inside components/menu; the
// linker stitches this reference. It moves here physically in M0c-2.
extern const machine_ui_t s2_sampler_menu_ui;

extern cJSON *s2_sampler_preset_save(void);
extern void s2_sampler_preset_load(const cJSON *node);

const machine_t s2_machine_sampler = {
    .name = "Sampler2",
    .start = sampler_start,
    .stop = sampler_stop,
    .process = sampler_process,
    .preset_save = s2_sampler_preset_save,
    .preset_load = s2_sampler_preset_load,
    .ui = &s2_sampler_menu_ui,
};

void s2_assignAudioFiles()
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
                        
                        s2_fill_audio_buffer(&voice[i].playback_engine, &audio_buffers[i * 3], &audio_files[i], NULL);
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

    // mirror the (possibly changed) filenames into the core status snapshot
    audio_status_set_voices(audio_files[0].fname, audio_files[1].fname);
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
    for (int j = 0; j < 8; j++)
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
    update_pitch_rows();

    for (int i = 0; i < 2; i++)
    {
        voice[i].voice_buffer = &audio_buffers[i * 3];
        s2_init_adsr(&voice[i].adsr);
        s2_init_ui_params(&ui_params[i]);
        voice[i].lowpass_filter = (Filter_t *)heap_caps_malloc(sizeof(Filter_t), MALLOC_CAP_INTERNAL);
        voice[i].highpass_filter = (Filter_t *)heap_caps_malloc(sizeof(Filter_t), MALLOC_CAP_INTERNAL);
        s2_init_play_mode(&voice[i].playback_engine);
        s2_init_filter(voice[i].lowpass_filter, lowpass);
        s2_init_filter(voice[i].highpass_filter, highpass);
        s2_init_fade(&voice[i]);
        s2_fifo_init(&voice_fifos[i], (int32_t *)heap_caps_calloc(BUF_SZ + 1, sizeof(int32_t), MALLOC_CAP_INTERNAL), BUF_SZ + 1);
    }

    bzero(s2_controlData, sizeof(unsigned int) * 8);
}



// ── queue binding + boot init (called from initUI before the core audio task
//    starts; menus move into this machine in M0c and this wiring goes away) ──

void s2_sampler_bind_queues(xQueueHandle v0, xQueueHandle v1, xQueueHandle eff,
                         xQueueHandle pbs0, xQueueHandle pbs1,
                         xQueueHandle mode0, xQueueHandle mode1,
                         xQueueHandle matrix_ev, xQueueHandle ui_ev)
{
    ui_parameter_queue_v0 = v0;
    ui_parameter_queue_v1 = v1;
    effect_parameter_queue = eff;
    playbackspeed_state_queue_v0 = pbs0;
    playbackspeed_state_queue_v1 = pbs1;
    mode_handle_v0 = mode0;
    mode_handle_v1 = mode1;
    m_event_queue = matrix_ev;
    ui_ev_queue = ui_ev;
}

void s2_sampler_boot_init(void)
{
    for (int i = 0; i < 2; i++)
    {
        file_mutex[i] = xSemaphoreCreateMutex();
        if (!file_mutex[i])
            ESP_LOGE("AUDIO", "Couldn't create mutex for audio file struct");
        buffer_mutex[i] = xSemaphoreCreateMutex();
        if (!buffer_mutex[i])
            ESP_LOGE("AUDIO", "Couldn't create mutex for audio buffer struct");
        counter_mutex[i] = xSemaphoreCreateMutex();
        if (!counter_mutex[i])
            ESP_LOGE("AUDIO", "Couldn't create mutex for buffer position counter");
    }
    initAudioStructs();
    s2_assignAudioFiles();
}

bool s2_isVoicePlaying(int vid) {
    if (vid < 0 || vid > 1) return false;
    return voice[vid].adsr.adsr_state != OFF;
}

void s2_audio_get_playpos(int vid, uint32_t *sample_start, uint32_t *loop_start, uint32_t *loop_end, uint32_t *fsize) {
    if (vid < 0 || vid > 1) return;
    *sample_start = voice[vid].playback_engine.sample_start;
    *loop_start   = voice[vid].playback_engine.loop_start;
    *loop_end     = voice[vid].playback_engine.loop_end;
    *fsize        = audio_files[vid].fsize;
}

void s2_getAudioBasename(int vid, char *out, int len) {
    if (vid < 0 || vid > 1 || len < 1) { if (len > 0) out[0] = '\0'; return; }
    const char *p = audio_files[vid].fname;
    if (!p || p[0] == '\0') { snprintf(out, len, "---"); return; }
    const char *slash = strrchr(p, '/');
    const char *base = slash ? slash + 1 : p;
    int blen = strlen(base);
    if (blen >= 4 && strcasecmp(base + blen - 4, ".RAW") == 0) blen -= 4;
    snprintf(out, len, "%.*s", blen, base);
}
