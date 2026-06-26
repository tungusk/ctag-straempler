#include "recording.h"
#include "fileio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#define TAG "REC"

#define REC_CHUNK_SAMPLES 64
#define REC_QUEUE_DEPTH 8

typedef struct {
    int32_t samples[REC_CHUNK_SAMPLES];
} rec_chunk_t;

static atomic_bool rec_active = ATOMIC_VAR_INIT(false);
static atomic_bool rec_load_pending = ATOMIC_VAR_INIT(false);
static trig_func_t trig_func[2] = {TRIG_FUNC_VOICE, TRIG_FUNC_VOICE};
static QueueHandle_t rec_queue = NULL;
static TaskHandle_t rec_task_handle = NULL;
static int rec_target_vid = 0;
static char rec_last_fname[48] = {0};

static void write_rec_jsn(const char *raw_path)
{
    const char *slash = strrchr(raw_path, '/');
    const char *base = slash ? slash + 1 : raw_path;
    char id[48];
    int baselen = strlen(base);
    if (baselen >= 4 && strcasecmp(base + baselen - 4, ".RAW") == 0)
        baselen -= 4;
    snprintf(id, sizeof(id), "%.*s", baselen, base);

    char jsn_path[64];
    snprintf(jsn_path, sizeof(jsn_path), "/sdcard/usr/%s.JSN", id);

    cJSON *root = cJSON_CreateObject();
    char name_field[52];
    snprintf(name_field, sizeof(name_field), "%s.raw", id);
    cJSON_AddStringToObject(root, "name", name_field);
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "description", "Recorded audio");
    cJSON_AddStringToObject(root, "tags_s", "recording");
    cJSON_AddStringToObject(root, "username", "myself");
    cJSON_AddStringToObject(root, "url", "local");
    cJSON_AddStringToObject(root, "license", "own license");

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (json_str) {
        writeJSONFile(jsn_path, json_str);
        free(json_str);
    }
    ESP_LOGI(TAG, "Wrote sidecar: %s", jsn_path);
}

static void find_next_filename(char *buf, int buflen)
{
    struct stat st;
    for (int i = 0; i < 9999; i++) {
        snprintf(buf, buflen, "/sdcard/usr/REC_%04d.RAW", i);
        if (stat(buf, &st) != 0) return;
    }
    snprintf(buf, buflen, "/sdcard/usr/REC_OVFL.RAW");
}

static void rec_writer_task(void *pvParams)
{
    char fname[48];
    find_next_filename(fname, sizeof(fname));

    FILE *f = fopen(fname, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Could not open %s for writing", fname);
        atomic_store(&rec_active, false);
        rec_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Recording to %s", fname);

    rec_chunk_t chunk;
    while (atomic_load(&rec_active) || uxQueueMessagesWaiting(rec_queue) > 0) {
        if (xQueueReceive(rec_queue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;

        // Convert stereo 32-bit I2S (left-justified 24-bit) to 16-bit packed RAW:
        // bits[15:0]=L, bits[31:16]=R
        int32_t packed[REC_CHUNK_SAMPLES / 2];
        for (int i = 0; i < REC_CHUNK_SAMPLES / 2; i++) {
            int16_t l = (int16_t)(chunk.samples[i * 2]     >> 16);
            int16_t r = (int16_t)(chunk.samples[i * 2 + 1] >> 16);
            packed[i] = (int32_t)(((uint32_t)(uint16_t)r << 16) | (uint16_t)l);
        }
        fwrite(packed, sizeof(int32_t), REC_CHUNK_SAMPLES / 2, f);
    }

    fclose(f);
    ESP_LOGI(TAG, "Recording saved: %s", fname);
    write_rec_jsn(fname);

    // Signal the audio task to load this file into the target voice slot
    memcpy(rec_last_fname, fname, sizeof(rec_last_fname) - 1);
    atomic_store(&rec_load_pending, true);

    rec_task_handle = NULL;
    vTaskDelete(NULL);
}

void recording_init(void)
{
    rec_queue = xQueueCreate(REC_QUEUE_DEPTH, sizeof(rec_chunk_t));
    if (rec_queue == NULL)
        ESP_LOGE(TAG, "Failed to create recording queue");
}

void recording_start(int vid)
{
    if (atomic_load(&rec_active)) return;
    if (rec_task_handle != NULL) return;
    if (rec_queue == NULL) return;
    rec_target_vid = vid;
    xQueueReset(rec_queue);
    atomic_store(&rec_active, true);
    xTaskCreate(rec_writer_task, "rec_writer", 4096, NULL, 18, &rec_task_handle);
    ESP_LOGI(TAG, "Recording started for vid=%d", vid);
}

void recording_stop(void)
{
    atomic_store(&rec_active, false);
    ESP_LOGI(TAG, "Recording stopped");
}

bool recording_is_active(void)
{
    return atomic_load(&rec_active);
}

bool recording_poll_load(int *vid_out, char *fname_out)
{
    if (!atomic_load(&rec_load_pending)) return false;
    atomic_store(&rec_load_pending, false);
    *vid_out = rec_target_vid;
    memcpy(fname_out, rec_last_fname, 48);
    fname_out[47] = '\0';
    return true;
}

void recording_set_trig_func(int vid, trig_func_t func)
{
    if (vid < 0 || vid > 1) return;
    trig_func[vid] = func;
    if (func == TRIG_FUNC_VOICE && atomic_load(&rec_active))
        recording_stop();
    ESP_LOGI(TAG, "TRIG%d -> %d", vid, (int)func);
}

trig_func_t recording_get_trig_func(int vid)
{
    if (vid < 0 || vid > 1) return TRIG_FUNC_VOICE;
    return trig_func[vid];
}

int recording_get_target_vid(void)
{
    return rec_target_vid;
}

void recording_push(const int32_t *samples)
{
    if (!atomic_load(&rec_active)) return;
    rec_chunk_t chunk;
    memcpy(chunk.samples, samples, sizeof(chunk.samples));
    if (xQueueSend(rec_queue, &chunk, 0) != pdTRUE)
        ESP_LOGD(TAG, "rec queue full, dropping chunk");
}
