// Deck offline analysis — estimate a track's BPM and beat-grid offset.
// Onset envelope (256-frame hops, ~172 Hz) -> half-wave-rectified flux ->
// autocorrelation over the 60..190 BPM lag range (80..165 preferred) with
// parabolic peak refinement -> grid phase by folding onsets into one beat.
// Results are committed to dk.track_bpm/grid_offset and cached in the
// track's JSN sidecar so each file is analysed once.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "fileio.h"
#include "sd_lock.h"
#include "deck_priv.h"

static const char *TAG = "DECK-AN";
#define ENV_RATE (44100.0f / DK_HOP)

void deck_analysis_commit(void)
{
    if (dk.an_state != DK_AN_DONE || dk.an_bpm <= 0) return;
    dk.track_bpm = dk.an_bpm;
    dk.grid_offset = dk.an_grid;

    char jp[64];
    snprintf(jp, sizeof(jp), "/sdcard/usr/%s.JSN", dk.track);
    cJSON *root = readJSONFileAsCJSON(jp);
    if (!root) root = cJSON_CreateObject();
    cJSON_DeleteItemFromObjectCaseSensitive(root, "bpm");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "grid");
    cJSON_AddNumberToObject(root, "bpm", dk.an_bpm);
    cJSON_AddNumberToObject(root, "grid", (double)dk.an_grid);
    char *s = cJSON_Print(root);
    cJSON_Delete(root);
    if (s) { writeJSONFile(jp, s); free(s); }
    ESP_LOGI(TAG, "%s: %.2f BPM, grid %lu (cached)", dk.track, dk.an_bpm, (unsigned long)dk.an_grid);
}

static void analysis_task(void *pv)
{
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/usr/%s.RAW", dk.track);

    float *env = heap_caps_malloc(DK_ENV_MAX * sizeof(float), MALLOC_CAP_SPIRAM);
    int16_t *chunk = malloc(DK_HOP * 2 * sizeof(int16_t));   // one hop per read
    sd_lock_take();
    FILE *f = fopen(path, "rb");
    long fsize = 0;
    if (f) { fseek(f, 0, SEEK_END); fsize = ftell(f); fseek(f, 0, SEEK_SET); }
    sd_lock_give();
    if (!env || !chunk || !f) {
        ESP_LOGE(TAG, "analysis setup failed");
        if (f) { sd_lock_take(); fclose(f); sd_lock_give(); }
        free(chunk);
        if (env) heap_caps_free(env);
        dk.an_state = DK_AN_FAIL;
        vTaskDelete(NULL);
        return;
    }

    // 1) onset envelope
    uint32_t total_hops = (uint32_t)(fsize / 4 / DK_HOP);
    if (total_hops > DK_ENV_MAX) total_hops = DK_ENV_MAX;   // cap ~5 min
    uint32_t n = 0;
    while (n < total_hops) {
        sd_lock_take();
        size_t got = fread(chunk, 4, DK_HOP, f);
        sd_lock_give();
        if (got < DK_HOP) break;
        int32_t acc = 0;
        for (int i = 0; i < DK_HOP; i++)
            acc += abs((int)chunk[i * 2]) + abs((int)chunk[i * 2 + 1]);
        env[n++] = (float)acc;
        if ((n & 0x3FF) == 0) {
            dk.an_progress = (int)((uint64_t)n * 70 / total_hops);
            vTaskDelay(1);           // let WiFi/audio breathe
        }
    }
    sd_lock_take();
    fclose(f);
    sd_lock_give();
    free(chunk);

    if (n < (uint32_t)(ENV_RATE * 10)) {         // need at least ~10 s
        heap_caps_free(env);
        ESP_LOGE(TAG, "track too short to analyse");
        dk.an_state = DK_AN_FAIL;
        vTaskDelete(NULL);
        return;
    }

    // 2) onset strength (half-wave rectified flux), in place
    for (uint32_t i = n - 1; i > 0; i--) {
        float d = env[i] - env[i - 1];
        env[i] = d > 0 ? d : 0;
    }
    env[0] = 0;

    // 3) autocorrelation over the BPM range, mild preference for 80..165
    int lag_min = (int)(ENV_RATE * 60.0f / 190.0f);   // ~54
    int lag_max = (int)(ENV_RATE * 60.0f / 60.0f);    // ~172
    float best = -1, r_prev = 0, r_best = 0, r_next = 0;
    int best_lag = 0;
    float *rr = malloc((lag_max + 2) * sizeof(float));
    for (int lag = lag_min; lag <= lag_max; lag++) {
        float acc = 0;
        for (uint32_t i = 0; i + lag < n; i++) acc += env[i] * env[i + lag];
        acc /= (float)(n - lag);
        rr[lag] = acc;
        float bpm = ENV_RATE * 60.0f / lag;
        float w = (bpm >= 80 && bpm <= 165) ? 1.0f : 0.7f;
        if (acc * w > best) { best = acc * w; best_lag = lag; }
        dk.an_progress = 70 + (lag - lag_min) * 25 / (lag_max - lag_min);
        if ((lag & 7) == 0) vTaskDelay(1);
    }
    r_best = rr[best_lag];
    r_prev = best_lag > lag_min ? rr[best_lag - 1] : r_best;
    r_next = best_lag < lag_max ? rr[best_lag + 1] : r_best;
    free(rr);

    // parabolic refinement around the peak
    float denom = r_prev - 2 * r_best + r_next;
    float shift = (denom != 0) ? 0.5f * (r_prev - r_next) / denom : 0;
    if (shift > 0.5f) shift = 0.5f;
    if (shift < -0.5f) shift = -0.5f;
    float lag_f = (float)best_lag + shift;
    float bpm = ENV_RATE * 60.0f / lag_f;

    // 4) grid phase: fold onsets into one beat period, strongest bin wins
    float P = lag_f;
    int BINS = 64;
    float acc_bin[64] = {0};
    for (uint32_t i = 0; i < n; i++) {
        int b = (int)(fmodf((float)i, P) / P * BINS);
        if (b >= 0 && b < BINS) acc_bin[b] += env[i];
    }
    int bb = 0;
    for (int b = 1; b < BINS; b++) if (acc_bin[b] > acc_bin[bb]) bb = b;
    float phase_env = ((float)bb + 0.5f) / BINS * P;          // env samples
    uint32_t grid = (uint32_t)(phase_env * DK_HOP);           // audio frames

    heap_caps_free(env);
    dk.an_bpm = bpm;
    dk.an_grid = grid;
    dk.an_progress = 100;
    dk.an_state = DK_AN_DONE;
    deck_analysis_commit();          // adopt + cache in the sidecar
    ESP_LOGI(TAG, "detected %.2f BPM (lag %.2f), grid %lu frames", bpm, lag_f, (unsigned long)grid);
    vTaskDelete(NULL);
}

int deck_analyze_start(void)
{
    if (!dk.track[0] || dk.an_state == DK_AN_RUNNING) return -1;
    dk.an_state = DK_AN_RUNNING;
    dk.an_progress = 0;
    // unpinned (reads files); modest priority so audio + reader stay smooth
    if (xTaskCreate(analysis_task, "deck_an", 6144, NULL, 4, NULL) != pdPASS) {
        dk.an_state = DK_AN_FAIL;
        return -1;
    }
    return 0;
}
