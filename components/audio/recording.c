#include "recording.h"
#include "sampfile.h"
#include "fileio.h"
#include "sd_lock.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

#define TAG "REC"

#define REC_CHUNK_SAMPLES 64
// ~6 s of capture buffering (PSRAM-backed static queue). The old depth of 8
// (5.8 ms!) dropped chunks — SILENTLY, at LOGD — whenever the sampler's
// reader held the SD bus longer than a block: every take recorded next to a
// streaming loop was perforated with clicks, worst at the take's end where
// the stamp write + head rebuild + auto-load pile onto the bus.
#define REC_QUEUE_DEPTH 1024
#define REC_BATCH 32               // chunks per fwrite: one sd_lock cycle per
                                   // 4 KB instead of per 128 B

typedef struct {
    int32_t samples[REC_CHUNK_SAMPLES];
} rec_chunk_t;

static atomic_bool rec_active = ATOMIC_VAR_INIT(false);
static atomic_bool rec_prepared = ATOMIC_VAR_INIT(false);
static atomic_bool rec_cancel = ATOMIC_VAR_INIT(false);
static atomic_bool rec_load_pending = ATOMIC_VAR_INIT(false);
static bool rec_enabled = true;
static trig_func_t trig_func[2] = {TRIG_FUNC_VOICE, TRIG_FUNC_VOICE};
static QueueHandle_t rec_queue = NULL;
static StaticQueue_t rec_queue_struct;
static uint8_t *rec_queue_store = NULL;
static TaskHandle_t rec_task_handle = NULL;
static int rec_target_vid = 0;
static char rec_last_fname[48] = {0};
static volatile uint32_t rec_drops = 0;   // dropped capture chunks (visible!)

static void write_rec_jsn(const char *raw_path)
{
    const char *slash = strrchr(raw_path, '/');
    const char *base = slash ? slash + 1 : raw_path;
    char id[48];
    int baselen = strlen(base);
    if (baselen >= 4 && (strcasecmp(base + baselen - 4, ".RAW") == 0 ||
                         strcasecmp(base + baselen - 4, ".WAV") == 0))
        baselen -= 4;
    snprintf(id, sizeof(id), "%.*s", baselen, base);

    char jsn_path[80];
    strlcpy(jsn_path, raw_path, sizeof(jsn_path));   // sidecar sits NEXT TO the take
    char *dot = strrchr(jsn_path, '.');
    if (dot && (size_t)(dot - jsn_path) + 5 < sizeof(jsn_path))
        memcpy(dot, ".JSN", 5);
    else
        snprintf(jsn_path, sizeof(jsn_path), "/sdcard/usr/REC/%.40s.JSN", id);

    cJSON *root = cJSON_CreateObject();
    char name_field[52];
    snprintf(name_field, sizeof(name_field), "%s.wav", id);
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
    // start from the last hit: REC numbers are monotonic within a boot, so
    // after the first scan this is 1-2 stats. The full from-zero scan (first
    // arm after boot) yields every few entries — each stat is a linear FAT
    // directory walk, and a few hundred of them back-to-back at this task's
    // priority starved the sampler reader (both tracks went silent) AND the
    // web server for seconds ("arming mutes both tracks").
    // numbering: one readdir MAX pass across every pool folder (legacy flat
    // usr/ RECs count — they must not shadow new usr/REC takes), then
    // monotonic within the boot. Takes land SORTED in usr/REC (2026-07-13
    // folder organization; pre-folder cards need no migration).
    static int hint = -1;
    mkdir("/sdcard/usr/REC", 0777);          // idempotent
    if (hint < 0) hint = sample_next_index("REC_");
    if (hint > 9999) { snprintf(buf, buflen, "/sdcard/usr/REC/REC_OVFL.WAV"); return; }
    snprintf(buf, buflen, "/sdcard/usr/REC/REC_%04d.WAV", hint);
    hint++;
}

static void rec_writer_task(void *pvParams)
{
    char fname[48];
    find_next_filename(fname, sizeof(fname));

    sd_lock_take();
    FILE *f = fopen(fname, "wb");
    if (f) sampwav_start(f);      // 44-byte header; sizes patched at finish
    sd_lock_give();
    if (f == NULL) {
        ESP_LOGE(TAG, "Could not open %s for writing", fname);
        atomic_store(&rec_active, false);
        atomic_store(&rec_prepared, false);
        rec_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Recording prepared: %s", fname);

    // park until the trigger (a clock pulse, via recording_trigger) or a
    // cancel (disarm before any capture — delete the empty file)
    while (!atomic_load(&rec_active) && !atomic_load(&rec_cancel))
        vTaskDelay(1);   // >=1 TICK: pdMS_TO_TICKS(5) is ZERO at 100Hz — a
                         // busy-spin that starved the reader + httpd while armed
    if (atomic_load(&rec_cancel)) {
        atomic_store(&rec_cancel, false);
        sd_lock_take();
        fclose(f);
        remove(fname);
        sd_lock_give();
        ESP_LOGI(TAG, "Prepared recording cancelled: %s", fname);
        atomic_store(&rec_prepared, false);
        rec_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Recording to %s", fname);

    rec_chunk_t chunk;
    bool write_err = false;
    size_t chunks_written = 0;
    // batch converter buffer: one fwrite (and one sd_lock cycle) per
    // REC_BATCH chunks instead of per chunk — the per-128-B lock churn made
    // capture lose every contention fight with the sampler's reader
    static int32_t batch[(REC_CHUNK_SAMPLES / 2) * REC_BATCH];
    int bn = 0;
    while (atomic_load(&rec_active) || uxQueueMessagesWaiting(rec_queue) > 0 || bn > 0) {
        bool got = (xQueueReceive(rec_queue, &chunk, pdMS_TO_TICKS(100)) == pdTRUE);
        if (got) {
            // Convert stereo 32-bit I2S (left-justified 24-bit) to 16-bit
            // packed RAW: bits[15:0]=L, bits[31:16]=R
            int32_t *dst = &batch[(REC_CHUNK_SAMPLES / 2) * bn];
            for (int i = 0; i < REC_CHUNK_SAMPLES / 2; i++) {
                int16_t l = (int16_t)(chunk.samples[i * 2]     >> 16);
                int16_t r = (int16_t)(chunk.samples[i * 2 + 1] >> 16);
                dst[i] = (int32_t)(((uint32_t)(uint16_t)r << 16) | (uint16_t)l);
            }
            bn++;
        }
        // flush on a full batch, on queue-idle timeout, or at stream end
        bool ending = !atomic_load(&rec_active) &&
                      uxQueueMessagesWaiting(rec_queue) == 0;
        if (bn > 0 && (bn >= REC_BATCH || !got || ending)) {
            size_t words = (size_t)(REC_CHUNK_SAMPLES / 2) * bn;
            sd_lock_take();
            size_t n = fwrite(batch, sizeof(int32_t), words, f);
            sd_lock_give();
            if (n != words) {
                // full card / IO error: stop consuming, keep what we have
                write_err = true;
                atomic_store(&rec_active, false);
                break;
            }
            chunks_written += bn;
            bn = 0;
        }
    }

    sd_lock_take();
    sampwav_finish(f);            // patch RIFF + data sizes from real length
    int close_err = fclose(f);
    sd_lock_give();

    if (write_err || close_err != 0 || chunks_written == 0) {
        // do NOT signal auto-load — a truncated/empty file must never be
        // handed to the machine as a fresh recording
        ESP_LOGE(TAG, "Recording save FAILED: %s (write_err=%d close_err=%d chunks=%u)",
                 fname, (int)write_err, close_err, (unsigned)chunks_written);
    } else {
        ESP_LOGI(TAG, "Recording saved: %s", fname);
        write_rec_jsn(fname);

        // Signal the machine to load this file into the target voice slot
        memcpy(rec_last_fname, fname, sizeof(rec_last_fname) - 1);
        atomic_store(&rec_load_pending, true);
    }

    atomic_store(&rec_prepared, false);
    rec_task_handle = NULL;
    vTaskDelete(NULL);
}

void recording_init(void)
{
    // deep capture queue lives in PSRAM (a static queue with internal-RAM
    // storage this size would eat the heap); items are memcpy'd, PSRAM is fine
    rec_queue_store = heap_caps_malloc((size_t)REC_QUEUE_DEPTH * sizeof(rec_chunk_t),
                                       MALLOC_CAP_SPIRAM);
    if (rec_queue_store)
        rec_queue = xQueueCreateStatic(REC_QUEUE_DEPTH, sizeof(rec_chunk_t),
                                       rec_queue_store, &rec_queue_struct);
    else
        rec_queue = xQueueCreate(64, sizeof(rec_chunk_t));   // degraded fallback
    if (rec_queue == NULL)
        ESP_LOGE(TAG, "Failed to create recording queue");
}

void recording_set_enabled(bool en) { rec_enabled = en; }
bool recording_get_enabled(void)    { return rec_enabled; }

// pass line-in through while a stopped voice is armed (pre-record cueing)
static bool rec_arm_monitor = true;
void recording_set_arm_monitor(bool en) { rec_arm_monitor = en; }
bool recording_get_arm_monitor(void)    { return rec_arm_monitor; }

void recording_prepare(int vid)
{
    if (!rec_enabled) return;
    if (atomic_load(&rec_prepared) || atomic_load(&rec_active)) return;
    if (rec_task_handle != NULL) return;
    if (rec_queue == NULL) return;
    rec_target_vid = vid;
    xQueueReset(rec_queue);
    atomic_store(&rec_cancel, false);
    atomic_store(&rec_prepared, true);
    // priority 10, not 18: the 6s capture queue absorbs scheduling slack,
    // and 18 let the prepare-phase directory scan starve the sampler reader
    // (prio 6) and httpd (prio 5) for seconds
    xTaskCreate(rec_writer_task, "rec_writer", 4096, NULL, 10, &rec_task_handle);
    ESP_LOGI(TAG, "Recording prepared for vid=%d", vid);
}

bool recording_is_prepared(void)
{
    return atomic_load(&rec_prepared);
}

// audio-task safe: bare atomic stores, no logs/SD/allocation
void recording_trigger(void)
{
    if (atomic_load(&rec_prepared)) atomic_store(&rec_active, true);
}

void recording_finish(void)
{
    atomic_store(&rec_active, false);
}

void recording_cancel_prepared(void)
{
    if (atomic_load(&rec_prepared) && !atomic_load(&rec_active))
        atomic_store(&rec_cancel, true);
}

void recording_start(int vid)
{
    recording_prepare(vid);
    recording_trigger();
    if (atomic_load(&rec_active)) ESP_LOGI(TAG, "Recording started for vid=%d", vid);
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
        rec_drops++;               // counted, surfaced via /status — a drop
                                   // is a click IN THE FILE, never silent
}

uint32_t recording_get_drops(void)
{
    return rec_drops;
}
