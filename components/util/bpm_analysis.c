// Offline BPM + beat-grid analysis (see bpm_analysis.h). Lifted from the deck's
// deck_analysis.c — the DSP is unchanged; the only edits decouple it from the
// deck singleton: the playback backpressure gate is now a busy() callback, the
// progress/result are out-params, and an abort flag lets a caller's stop() bail
// a run out. Onset envelope (256-frame hops, ~172 Hz) -> half-wave-rectified
// flux -> autocorrelation over 60..190 BPM (80..165 preferred) with harmonic
// disambiguation, parabolic + long-lag ladder refinement -> grid phase by
// folding onsets into one beat with sub-bin interpolation.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ff.h"
#include "sd_lock.h"
#include "sampfile_f.h"
#include "bpm_analysis.h"

static const char *TAG = "BPM-AN";
#define ENV_RATE (44100.0f / BPM_HOP)

// only ONE analysis runs at a time (one active machine, and each caller guards
// re-entry), so the busy hook + abort flag can be file-static.
static bool (*s_busy)(void) = NULL;
static volatile bool s_abort = false;

void bpm_analyze_abort(void) { s_abort = true; }

// pause while the caller owns the SD bus; return false if we were aborted mid-wait
static bool wait_idle(void)
{
    while (s_busy && s_busy()) {
        if (s_abort) return false;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return !s_abort;
}

// one ACF evaluation at an arbitrary lag (pauses while the caller plays)
static float acf_at(const float *env, uint32_t n, uint32_t lag)
{
    if (!wait_idle()) return 0;
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

int bpm_analyze(const char *id, bool (*busy)(void), volatile int *progress, bpm_result_t *out)
{
    s_busy = busy;
    s_abort = false;
    if (progress) *progress = 0;

    char path[64];
    if (sample_resolve_f(id, path, sizeof(path)) != 0) {   // .RAW/.WAV/.AIF(F)
        ESP_LOGE(TAG, "resolve failed: %s", id);
        return -1;
    }
    ESP_LOGI(TAG, "analysis start: %s (heap %u, largest %u)", id,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    float *env = heap_caps_malloc(BPM_ENV_MAX * sizeof(float), MALLOC_CAP_SPIRAM);
    // 16 hops per read (16 KB): 1 KB reads made analysis crawl (~30k SD
    // round-trips competing with the playback reader for sd_lock). DMA-capable
    // internal RAM per the SD house rule.
    #define AN_CHUNK_HOPS 16
    int16_t *chunk = heap_caps_malloc(AN_CHUNK_HOPS * BPM_HOP * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    FIL *f = malloc(sizeof(FIL));    // FatFS handle off the task stack
    sampfile_t sfa = {0};
    bool open_ok = false;
    if (f) {
        sd_lock_take();
        open_ok = (f_open(f, path, FA_READ) == FR_OK);
        if (open_ok && sampfile_probe_f(f, &sfa) != 0) {
            ESP_LOGE(TAG, "%s: %s", path, sfa.why);
            f_close(f);
            open_ok = false;
        }
        sd_lock_give();
    }
    if (!env || !chunk || !open_ok) {
        ESP_LOGE(TAG, "analysis setup failed");
        if (open_ok) { sd_lock_take(); f_close(f); sd_lock_give(); }
        free(f);
        free(chunk);
        if (env) heap_caps_free(env);
        return -1;
    }

    // 1) onset envelope
    uint32_t total_hops = sfa.frames / BPM_HOP;
    if (total_hops > BPM_ENV_MAX) total_hops = BPM_ENV_MAX;   // cap ~5 min
    uint32_t n = 0;
    bool aborted = false;
    while (n < total_hops) {
        // ANALYSE ONLY WHILE STOPPED: pause entirely during playback so we never
        // touch the SD bus while a ring reader needs it — that contention was
        // what hurt the audio.
        if (!wait_idle()) { aborted = true; break; }
        uint32_t hops = total_hops - n;
        if (hops > AN_CHUNK_HOPS) hops = AN_CHUNK_HOPS;
        sd_lock_take();
        size_t got_fr = sampfile_read_f(f, &sfa, chunk, hops * BPM_HOP);
        sd_lock_give();
        uint32_t got_hops = (uint32_t)(got_fr / BPM_HOP);
        for (uint32_t h = 0; h < got_hops && n < total_hops; h++) {
            int32_t acc = 0;
            const int16_t *p = chunk + h * BPM_HOP * 2;
            for (int i = 0; i < BPM_HOP; i++)
                acc += abs((int)p[i * 2]) + abs((int)p[i * 2 + 1]);
            env[n++] = (float)acc;
        }
        if (progress) *progress = (int)((uint64_t)n * 70 / total_hops);
        if (got_hops < hops) break;      // EOF/short read
        // per-chunk pace (this gap is load-bearing — removing it thrashed
        // sd_lock/scheduling and made analysis SLOWER, not faster)
        vTaskDelay(1);
    }
    sd_lock_take();
    f_close(f);
    sd_lock_give();
    free(f);
    free(chunk);
    if (aborted) { heap_caps_free(env); return -2; }

    if (n < (uint32_t)(ENV_RATE * 10)) {         // need at least ~10 s
        heap_caps_free(env);
        ESP_LOGE(TAG, "track too short to analyse");
        return -1;
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
    if (!rr) { heap_caps_free(env); return -1; }
    for (int lag = lag_min; lag <= lag_max; lag++) {
        if (!wait_idle()) { free(rr); heap_caps_free(env); return -2; }
        float acc = 0;
        for (uint32_t i = 0; i + lag < n; i++) acc += env[i] * env[i + lag];
        acc /= (float)(n - lag);
        rr[lag] = acc;
        float bpm = ENV_RATE * 60.0f / lag;
        float w = (bpm >= 80 && bpm <= 165) ? 1.0f : 0.7f;
        if (acc * w > best) { best = acc * w; best_lag = lag; }
        if (progress) *progress = 70 + (lag - lag_min) * 20 / (lag_max - lag_min);
        if ((lag & 7) == 0) vTaskDelay(1);   // the periodic yield is load-bearing
    }

    // 3b) harmonic disambiguation: a strong half-beat pulse can park the raw
    // peak at half/double the true beat lag. The true beat lag also scores at
    // its double, a spurious one doesn't: score = acf(c) + 0.5*acf(2c).
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

    // 3c) peak salience -> confidence
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

    // 3d) long-lag refinement ladder: re-find the ACF peak at k beats and divide
    // the +/-0.5-lag quantization by k (k=256 -> ~0.003 BPM at 120). No second
    // file pass — works on the envelope already in PSRAM.
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
        if (s_abort) { heap_caps_free(env); return -2; }
        int bi = lb - center + 8;
        float rp = bi > 0 ? rv[bi - 1] : rb;
        float rn = bi < 16 ? rv[bi + 1] : rb;
        float dn = rp - 2 * rb + rn;
        float sh = (dn != 0) ? 0.5f * (rp - rn) / dn : 0;
        if (sh > 0.5f) sh = 0.5f;
        if (sh < -0.5f) sh = -0.5f;
        L = ((double)lb + sh) / (double)k;
        if (progress) *progress = 90 + (ki + 1) * 7 / 3;
    }
    float bpm = ENV_RATE * 60.0f / (float)L;

    // 4) grid phase: fold onsets into one beat period, strongest bin wins,
    // circular parabolic interpolation between bins refines the anchor
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
    uint32_t grid = (uint32_t)(phase_env * BPM_HOP);           // audio frames

    heap_caps_free(env);
    if (progress) *progress = 100;
    out->bpm = bpm;
    out->grid = grid;
    out->conf = conf;
    ESP_LOGI(TAG, "detected %.4f BPM (lag %.4f, conf %.2f), grid %lu frames",
             bpm, L, conf, (unsigned long)grid);
    return 0;
}
