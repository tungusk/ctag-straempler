// Deck offline analysis — estimate a track's BPM and beat-grid offset.
// Onset envelope (256-frame hops, ~172 Hz) -> half-wave-rectified flux ->
// autocorrelation over the 60..190 BPM lag range (80..165 preferred) with
// harmonic disambiguation, parabolic refinement, and a long-lag refinement
// ladder (re-peak at 8/64/256 beats: each rung divides the +/-0.5-lag
// quantization by k, reaching ~0.003 BPM at 120) -> grid phase by folding
// onsets into one beat with sub-bin interpolation. Results are committed to
// dk.track_bpm/grid_offset and cached in the track's JSN sidecar (v2:
// "dver"/"conf"; the PLL cannot see track-BPM error — p_trk derives from the
// same seg_tf — so the audible lock quality is set entirely here).
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

// the track this run is analysing, snapshotted at start: a new load can
// race a running analysis, and resolving dk.track at commit time stamped
// the OLD track's bpm into the NEW track's sidecar
static char s_an_track[DK_NAME_LEN];

void deck_analysis_commit(void)
{
    if (dk.an_state != DK_AN_DONE || dk.an_bpm <= 0) return;
    if (strcmp(s_an_track, dk.track) == 0) {   // adopt live only if still loaded
        dk.track_bpm = dk.an_bpm;
        dk.grid_offset = dk.an_grid;
    }

    char jp[64];
    snprintf(jp, sizeof(jp), "/sdcard/usr/%s.JSN", s_an_track);
    cJSON *root = readJSONFileAsCJSON(jp);
    if (!root) root = cJSON_CreateObject();
    // sidecar v2: "dver" versions the analysis (missing = v1 -> auto-upgrade
    // on load), "conf" = ACF peak salience. Reserved for a future tempo-map:
    // "anchors": [[frame, beat_index], ...] — readers must ignore it unknown.
    cJSON_DeleteItemFromObjectCaseSensitive(root, "bpm");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "grid");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "dver");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "conf");
    cJSON_AddNumberToObject(root, "bpm", dk.an_bpm);
    cJSON_AddNumberToObject(root, "grid", (double)dk.an_grid);
    cJSON_AddNumberToObject(root, "dver", 2);
    cJSON_AddNumberToObject(root, "conf", (double)dk.an_conf);
    char *s = cJSON_Print(root);
    cJSON_Delete(root);
    if (s) { writeJSONFile(jp, s); free(s); }
    ESP_LOGI(TAG, "%s: %.4f BPM (conf %.2f), grid %lu (cached)", s_an_track, dk.an_bpm, dk.an_conf, (unsigned long)dk.an_grid);
}

// one ACF evaluation at an arbitrary lag (pauses while the deck plays —
// same rule as the main sweep)
static float acf_at(const float *env, uint32_t n, uint32_t lag)
{
    while (dk.playing) vTaskDelay(pdMS_TO_TICKS(30));
    if (lag == 0 || lag >= n) return 0;
    float acc = 0;
    for (uint32_t i = 0; i + lag < n; i++) acc += env[i] * env[i + lag];
    return acc / (float)(n - lag);
}

static int cmp_float(const void *a, const void *b)
{
    float d = *(const float *)a - *(const float *)b;
    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

static void analysis_task(void *pv)
{
    char path[64];
    ESP_LOGI(TAG, "analysis start: %s (heap %u, largest %u)", s_an_track,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    snprintf(path, sizeof(path), "/sdcard/usr/%s.RAW", s_an_track);

    float *env = heap_caps_malloc(DK_ENV_MAX * sizeof(float), MALLOC_CAP_SPIRAM);
    // 16 hops per read (16 KB): 1 KB reads made analysis crawl (~30k SD
    // round-trips competing with the playback reader for sd_lock).
    // DMA-capable internal RAM per the SD house rule (with CAPS_ALLOC plain
    // malloc is internal anyway, but be explicit). NB the "analysis crawl"
    // (fread 30 ms idle -> ~900 ms while the deck is in active use, measured
    // via the heartbeat buckets) is NOT buffer placement — root cause still
    // open; see the bucket logs. Idle-speed analysis is ~2.25 min / 4.5 min
    // track and reads dominate (~31 ms per 16 KB through the VFS path).
    #define AN_CHUNK_HOPS 16
    int16_t *chunk = heap_caps_malloc(AN_CHUNK_HOPS * DK_HOP * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
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
    // where does chunk time go? (ticks; printed per heartbeat, then reset)
    uint32_t tk_lock = 0, tk_read = 0, tk_rest = 0, tk_last = xTaskGetTickCount();
    while (n < total_hops) {
        // ANALYSE ONLY WHILE STOPPED: pause entirely during playback so we never
        // touch the SD bus while the ring reader needs it — that contention was
        // what hurt the audio.
        while (dk.playing) vTaskDelay(pdMS_TO_TICKS(30));
        uint32_t hops = total_hops - n;
        if (hops > AN_CHUNK_HOPS) hops = AN_CHUNK_HOPS;
        uint32_t t0 = xTaskGetTickCount();
        sd_lock_take();
        uint32_t t1 = xTaskGetTickCount();
        size_t got = fread(chunk, 4, hops * DK_HOP, f);
        uint32_t t2 = xTaskGetTickCount();
        sd_lock_give();
        tk_lock += t1 - t0;
        tk_read += t2 - t1;
        tk_rest += t0 - tk_last;
        tk_last = t2;
        uint32_t got_hops = got / DK_HOP;
        for (uint32_t h = 0; h < got_hops && n < total_hops; h++) {
            int32_t acc = 0;
            const int16_t *p = chunk + h * DK_HOP * 2;
            for (int i = 0; i < DK_HOP; i++)
                acc += abs((int)p[i * 2]) + abs((int)p[i * 2 + 1]);
            env[n++] = (float)acc;
        }
        dk.an_progress = (int)((uint64_t)n * 70 / total_hops);
        if ((n & 4095) < AN_CHUNK_HOPS) {                  // ~every 256 chunks
            ESP_LOGI(TAG, "env %lu/%lu hops (%.1f s) lock %lu read %lu rest %lu ms",
                     (unsigned long)n, (unsigned long)total_hops,
                     (double)(xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000.0,
                     (unsigned long)(tk_lock * portTICK_PERIOD_MS),
                     (unsigned long)(tk_read * portTICK_PERIOD_MS),
                     (unsigned long)(tk_rest * portTICK_PERIOD_MS));
            tk_lock = tk_read = tk_rest = 0;
        }
        if (got_hops < hops) break;      // EOF/short read
        // per-chunk pace (this gap is load-bearing — removing it thrashed
        // sd_lock/scheduling and made analysis SLOWER, not faster)
        vTaskDelay(1);
    }
    ESP_LOGI(TAG, "envelope done: %lu hops (%.1f s)", (unsigned long)n,
             (double)(xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000.0);
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
        while (dk.playing) vTaskDelay(pdMS_TO_TICKS(30));   // pause during playback
        float acc = 0;
        for (uint32_t i = 0; i + lag < n; i++) acc += env[i] * env[i + lag];
        acc /= (float)(n - lag);
        rr[lag] = acc;
        float bpm = ENV_RATE * 60.0f / lag;
        float w = (bpm >= 80 && bpm <= 165) ? 1.0f : 0.7f;
        if (acc * w > best) { best = acc * w; best_lag = lag; }
        dk.an_progress = 70 + (lag - lag_min) * 20 / (lag_max - lag_min);
        if ((lag & 7) == 0) {
            ESP_LOGI(TAG, "acf lag %d/%d (%.1f s)", lag, lag_max,
                     (double)(xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000.0);
            vTaskDelay(1);
        }
    }
    ESP_LOGI(TAG, "coarse done, harmonic check");

    // 3b) harmonic disambiguation: a strong half-beat pulse can park the raw
    // peak at half/double the true beat lag. The true beat lag also scores
    // at its double, a spurious one doesn't: score = acf(c) + 0.5*acf(2c).
    {
        int cands[3];
        int nc = 0;
        cands[nc++] = best_lag;
        if (best_lag / 2 >= lag_min) cands[nc++] = best_lag / 2;
        if (best_lag * 2 <= lag_max) cands[nc++] = best_lag * 2;
        float sc_best = -1;
        int chosen = best_lag;
        for (int ci = 0; ci < nc; ci++) {
            int c = cands[ci];
            // snap onto the local rr peak — integer halving can land 1 off
            while (c > lag_min && rr[c - 1] > rr[c]) c--;
            while (c < lag_max && rr[c + 1] > rr[c]) c++;
            float a2 = (2 * c <= lag_max) ? rr[2 * c] : acf_at(env, n, 2 * c);
            float bpmc = ENV_RATE * 60.0f / c;
            float w = (bpmc >= 80 && bpmc <= 165) ? 1.0f : 0.7f;
            float sc = w * (rr[c] + 0.5f * a2);
            if (sc > sc_best) { sc_best = sc; chosen = c; }
        }
        best_lag = chosen;
    }

    // 3c) peak salience -> confidence: a clean constant tempo shows one
    // dominant ACF peak; warped/rubato material flattens it out
    float conf = 0;
    {
        int nl = lag_max - lag_min + 1;
        float *srt = malloc(nl * sizeof(float));
        if (srt) {
            memcpy(srt, rr + lag_min, nl * sizeof(float));
            qsort(srt, nl, sizeof(float), cmp_float);
            if (rr[best_lag] > 0) conf = 1.0f - srt[nl / 2] / rr[best_lag];
            if (conf < 0) conf = 0;
            if (conf > 1) conf = 1;
            free(srt);
        }
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

    // 3d) long-lag refinement ladder: re-find the ACF peak at k beats and
    // divide the +/-0.5-lag quantization by k (k=256 -> ~0.003 BPM at 120).
    // No second file pass — works on the envelope already in PSRAM. The PLL
    // can't correct track-BPM error (it locks the model grid, and p_trk is
    // derived from the same model), so playback slips audio-vs-clock at
    // exactly the analysis residual: this ladder is the tempo-tightness fix.
    double L = lag_f;
    static const int ks[3] = { 8, 64, 256 };
    for (int ki = 0; ki < 3; ki++) {
        int k = ks[ki];
        double center_f = (double)k * L;
        if (center_f > (double)n * 0.5) break;   // not enough overlap left
        int center = (int)(center_f + 0.5);
        float rv[17];
        float rb = -1;
        int lb = center;
        for (int d = -8; d <= 8; d++) {
            rv[d + 8] = acf_at(env, n, (uint32_t)(center + d));
            if (rv[d + 8] > rb) { rb = rv[d + 8]; lb = center + d; }
            if ((d & 7) == 0) vTaskDelay(1);
        }
        int bi = lb - center + 8;
        float rp = bi > 0 ? rv[bi - 1] : rb;
        float rn = bi < 16 ? rv[bi + 1] : rb;
        float dn = rp - 2 * rb + rn;
        float sh = (dn != 0) ? 0.5f * (rp - rn) / dn : 0;
        if (sh > 0.5f) sh = 0.5f;
        if (sh < -0.5f) sh = -0.5f;
        L = ((double)lb + sh) / (double)k;
        dk.an_progress = 90 + (ki + 1) * 7 / 3;
    }
    float bpm = ENV_RATE * 60.0f / (float)L;

    // 4) grid phase: fold onsets into one beat period (using the final
    // refined period — a 0.003% error smears ~1 hop over 5 min), strongest
    // bin wins; circular parabolic interpolation between bins takes the
    // anchor from ~P/64 (~8 ms at 120) to ~1-2 ms
    float P = (float)L;
    int BINS = 64;
    float acc_bin[64] = {0};
    for (uint32_t i = 0; i < n; i++) {
        int b = (int)(fmodf((float)i, P) / P * BINS);
        if (b >= 0 && b < BINS) acc_bin[b] += env[i];
    }
    int bb = 0;
    for (int b = 1; b < BINS; b++) if (acc_bin[b] > acc_bin[bb]) bb = b;
    float ap = acc_bin[(bb + BINS - 1) % BINS];
    float an2 = acc_bin[(bb + 1) % BINS];
    float dnb = ap - 2 * acc_bin[bb] + an2;
    float shb = (dnb != 0) ? 0.5f * (ap - an2) / dnb : 0;
    if (shb > 0.5f) shb = 0.5f;
    if (shb < -0.5f) shb = -0.5f;
    float phase_env = ((float)bb + 0.5f + shb) / BINS * P;    // env samples
    if (phase_env < 0) phase_env += P;
    uint32_t grid = (uint32_t)(phase_env * DK_HOP);           // audio frames

    heap_caps_free(env);
    dk.an_bpm = bpm;
    dk.an_grid = grid;
    dk.an_conf = conf;
    dk.an_progress = 100;
    dk.an_state = DK_AN_DONE;
    deck_analysis_commit();          // adopt + cache in the sidecar
    ESP_LOGI(TAG, "detected %.4f BPM (lag %.4f, conf %.2f), grid %lu frames (stack low-water %u B)",
             bpm, L, conf, (unsigned long)grid, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

int deck_analyze_start(void)
{
    if (!dk.track[0]) return -1;
    if (dk.an_state == DK_AN_RUNNING) {
        ESP_LOGW(TAG, "analyze_start: already running (%s)", s_an_track);
        return -1;
    }
    strlcpy(s_an_track, dk.track, sizeof(s_an_track));
    dk.an_state = DK_AN_RUNNING;
    dk.an_progress = 0;
    // unpinned (reads files); modest priority so audio + reader stay smooth
    if (xTaskCreate(analysis_task, "deck_an", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "analyze_start: xTaskCreate FAILED (heap %u, largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        dk.an_state = DK_AN_FAIL;
        return -1;
    }
    return 0;
}
