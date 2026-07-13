// Sampler3 engine — two gate-triggered voices streamed from SD through
// per-voice PSRAM head-cache + ring (see sampler3_priv.h for the model).
// The reader task owns ALL file I/O; process() only consumes buffers and
// flips request flags. Recording start/stop is also deferred to the reader
// (task creation allocates — not allowed in the audio callback).
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sample_ram.h"  // shared dated browser walk (sample_list_recent)
#include "machine.h"
#include "audio.h"
#include "recording.h"
#include "sd_lock.h"
#include "fileio.h"
#include "sampfile.h"
#include "sampler3_priv.h"

static const char *TAG = "S3";

s3_state_t s3;

static volatile bool s_run = false, s_alive = false;
// audio task -> reader for the gate workflow's SLOW actions (arm = SD prepare,
// abort = stop+log). The fast actions — recording_trigger/finish on a clock
// pulse — are bare atomics and run directly in the audio task.
static volatile bool s_arm_req = false;        // ~1s gate hold: arm that voice
static volatile int  s_arm_vid = 0;
static volatile bool s_rec_abort_req = false;  // hold during a take: stop+disarm+discard
static volatile bool s_discard_next = false;   // eat the aborted take's auto-pickup

// ---- reader task -------------------------------------------------------------

#define S3_CHUNK_FRAMES 4096
#define S3_LOW_WATER    (S3_RATE / 4)     // unmute once 0.25 s is buffered
#define S3_XFADE_FRAMES 1024              // loop-seam crossfade (~23 ms): a
                                          // LIGHT musical fade, not just a
                                          // declick (Arlo). Clamped to
                                          // window/4 for tiny windows.

typedef struct {
    FILE *f;
    sampfile_t sf;          // container descriptor (probe at open)
    uint32_t stream_p;      // next playback-order frame the stream will read
} s3_reader_voice_t;

// read `n` playback-order frames starting at frame `p` of voice v into dst
// (interleaved stereo int16). Handles the forward/reverse file mapping.
// Caller provides the DMA staging buffer. Returns frames actually read.
static uint32_t read_playback_frames(s3_voice_t *v, FILE *f,
                                     const sampfile_t *sf, uint32_t p,
                                     uint32_t n, int16_t *dst, int16_t *stage)
{
    if (p >= v->play_len) return 0;
    if (n > v->play_len - p) n = v->play_len - p;
    uint32_t got_total = 0;
    while (got_total < n) {
        uint32_t want = n - got_total;
        if (want > S3_CHUNK_FRAMES) want = S3_CHUNK_FRAMES;
        uint32_t pp = p + got_total;
        long file_frame = v->reverse
            ? (long)v->play_start + (long)v->play_len - 1 - (long)pp - (long)(want - 1)
            : (long)v->play_start + (long)pp;
        if (file_frame < 0) { want += file_frame; file_frame = 0; }
        sd_lock_take();
        fseek(f, sf_seek_pos(sf, (uint32_t)file_frame), SEEK_SET);
        size_t got = sampfile_read(f, sf, stage, want);
        sd_lock_give();
        if (got == 0) break;
        if (v->reverse) {
            for (uint32_t i = 0; i < got; i++) {
                dst[(got_total + i) * 2]     = stage[(got - 1 - i) * 2];
                dst[(got_total + i) * 2 + 1] = stage[(got - 1 - i) * 2 + 1];
            }
        } else {
            memcpy(dst + got_total * 2, stage, got * 4);
        }
        got_total += got;
        if (got < want) break;
    }
    return got_total;
}

// (re)apply the trim window and rebuild the head cache. Engine is parked on
// head_valid=false/loading while this runs.
static void rebuild_head(s3_voice_t *v, s3_reader_voice_t *rv, int16_t *stage)
{
    v->head_valid = false;
    v->lsc_valid = false;       // playback order may have changed (reverse/
    v->lsc_start = 0;           // load): the loop-start cache is stale
    // the reader always streams the WHOLE file; crop is engine-side cursor
    // math (CV-performable — never lands back here)
    v->play_start = 0;
    v->play_len = v->file_frames;
    uint32_t hf = v->play_len < S3_HEAD_FRAMES ? v->play_len : S3_HEAD_FRAMES;
    uint32_t got = 0;
    // head is PSRAM: stream through the DMA stage in chunks
    while (got < hf) {
        uint32_t want = hf - got;
        if (want > S3_CHUNK_FRAMES) want = S3_CHUNK_FRAMES;
        uint32_t r = read_playback_frames(v, rv->f, &rv->sf, got, want, stage, stage);
        if (r == 0) break;
        memcpy(v->head + got * 2, stage, r * 4);
        got += r;
        vTaskDelay(1);          // SD courtesy gap (tracker pattern)
    }
    v->head_frames = got;
    v->wpos = got;              // ring restarts empty right after the head
    rv->stream_p = got;
    v->head_valid = true;

    // waveform thumbnail: one small decimated pass in playback order (so
    // reverse mode draws reversed). ~144 seek+peek reads — cheap, and only
    // on load/window change.
    v->wf_valid = false;
    for (int c = 0; c < S3_WF_W; c++) {
        uint32_t p = (uint32_t)((uint64_t)c * v->play_len / S3_WF_W);
        uint32_t want = v->play_len - p;
        if (want > 128) want = 128;
        uint32_t got_c = want ? read_playback_frames(v, rv->f, &rv->sf, p, want, stage, stage) : 0;
        int peak = 0;
        for (uint32_t k = 0; k < got_c * 2; k++) {
            int s = stage[k];
            if (s < 0) s = -s;
            if (s > peak) peak = s;
        }
        v->wf[c] = (uint8_t)(peak >> 7);
        if ((c & 15) == 15) vTaskDelay(1);   // SD courtesy gap
    }
    v->wf_valid = true;
}

static void reader_task(void *pv)
{
    s3_reader_voice_t rv[S3_NVOICES] = {0};
    int16_t *stage = heap_caps_malloc(S3_CHUNK_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    s_alive = true;

    while (s_run) {
        bool worked = false;

        // ~1s gate hold while idle = arm that voice for recording
        if (s_arm_req) {
            s_arm_req = false;
            if (!recording_is_active()) {
                s3.save_failed = false;
                s3_toggle_arm(s_arm_vid);
            }
            worked = true;
        }

        // ~1s gate hold mid-take = abort: stop, disarm, discard the pickup
        if (s_rec_abort_req) {
            s_rec_abort_req = false;
            int vid = recording_get_target_vid();
            s3.rec_wait_vid = -1;
            s3.rec_stop_wait = false;
            if (recording_is_active()) {
                s_discard_next = true;
                recording_stop();
                ESP_LOGI(TAG, "take aborted (gate hold)");
            } else {
                recording_cancel_prepared();
            }
            recording_set_trig_func(vid, TRIG_FUNC_VOICE);
            s3.arm_target = -1;
            worked = true;
        }

        // finished recording -> auto-pickup into the target voice
        {
            int vid; char fname[48];
            if (recording_poll_load(&vid, fname)) {
                // stop/finish may have come from the audio task — disarm here
                if (vid >= 0 && vid < S3_NVOICES &&
                    recording_get_trig_func(vid) == TRIG_FUNC_RECORD) {
                    recording_set_trig_func(vid, TRIG_FUNC_VOICE);
                    s3.arm_target = -1;
                }
                if (s_discard_next) {
                    s_discard_next = false;
                    s3.rec_stamp_req = false;
                    ESP_LOGI(TAG, "discarded aborted take %s", fname);
                } else {
                    const char *slash = strrchr(fname, '/');
                    const char *base = slash ? slash + 1 : fname;
                    char id[S3_NAME_LEN];
                    int blen = strlen(base);
                    if (blen >= 4 && strcasecmp(base + blen - 4, ".RAW") == 0) blen -= 4;
                    snprintf(id, sizeof(id), "%.*s", blen, base);
                    // tempo stamp: a clock was locked during the take — write
                    // bpm + grid into the sidecar (deck convention, dver 2,
                    // conf 1.0: clock-derived beats estimation). Takes arrive
                    // pre-analyzed for the deck; synced starts have grid 0.
                    if (s3.rec_stamp_req) {
                        s3.rec_stamp_req = false;
                        char jp[64];
                        snprintf(jp, sizeof(jp), "/sdcard/usr/%s.JSN", id);
                        cJSON *root = readJSONFileAsCJSON(jp);
                        if (!root) root = cJSON_CreateObject();
                        cJSON_DeleteItemFromObjectCaseSensitive(root, "bpm");
                        cJSON_DeleteItemFromObjectCaseSensitive(root, "grid");
                        cJSON_DeleteItemFromObjectCaseSensitive(root, "dver");
                        cJSON_DeleteItemFromObjectCaseSensitive(root, "conf");
                        cJSON_AddNumberToObject(root, "bpm", (double)s3.rec_bpm);
                        cJSON_AddNumberToObject(root, "grid",
                            s3.rec_synced ? 0 : (double)s3.rec_first_pulse);
                        cJSON_AddNumberToObject(root, "dver", 2);
                        cJSON_AddNumberToObject(root, "conf", 1.0);
                        char *str = cJSON_Print(root);
                        if (str) { writeJSONFile(jp, str); free(str); }
                        cJSON_Delete(root);
                        ESP_LOGI(TAG, "tempo stamp %s: %.1f bpm grid %lu%s", id,
                                 s3.rec_bpm,
                                 (unsigned long)(s3.rec_synced ? 0 : s3.rec_first_pulse),
                                 s3.rec_synced ? " (clock-synced take)" : "");
                    }
                    if (vid >= 0 && vid < S3_NVOICES) {
                        ESP_LOGI(TAG, "auto-pickup %s -> voice %d", id, vid);
                        strlcpy(s3.v[vid].pending, id, S3_NAME_LEN);
                        s3.v[vid].autoplay = true;    // fresh take loops right away
                        s3.v[vid].load_req = true;
                        strlcpy(s3.last_rec, id, S3_NAME_LEN);
                    }
                }
                worked = true;
            }
        }

        for (int i = 0; i < S3_NVOICES; i++) {
            s3_voice_t *v = &s3.v[i];

            if (v->load_req) {
                v->load_req = false;
                v->playing = false;
                v->head_valid = false;
                if (rv[i].f) { sd_lock_take(); fclose(rv[i].f); sd_lock_give(); rv[i].f = NULL; }
                v->name[0] = 0;
                v->file_frames = 0;
                if (v->pending[0]) {
                    char path[64];
                    sample_resolve(v->pending, path, sizeof(path));
                    sd_lock_take();
                    FILE *f = fopen(path, "rb");
                    if (f) {
                        if (sampfile_probe(f, &rv[i].sf) == 0)
                            v->file_frames = rv[i].sf.frames;
                        else {
                            ESP_LOGE(TAG, "%s: %s", path, rv[i].sf.why);
                            fclose(f); f = NULL;
                        }
                    }
                    sd_lock_give();
                    if (!f || v->file_frames == 0) {
                        // missing/empty file = voice stays unloaded, never abort
                        ESP_LOGE(TAG, "open %s failed", path);
                        if (f) { sd_lock_take(); fclose(f); sd_lock_give(); }
                    } else {
                        rv[i].f = f;
                        strlcpy(v->name, v->pending, S3_NAME_LEN);
                        rebuild_head(v, &rv[i], stage);
                        // take tempo from the sidecar (every load path lands
                        // here: browse, preset restore, record pickup) — the
                        // QUANT crop mode snaps its points to these beats
                        v->bpm = 0;
                        char jp[64];
                        snprintf(jp, sizeof(jp), "/sdcard/usr/%s.JSN", v->name);
                        cJSON *root = readJSONFileAsCJSON(jp);
                        if (root) {
                            cJSON *jb = cJSON_GetObjectItemCaseSensitive(root, "bpm");
                            if (jb && cJSON_IsNumber(jb) && jb->valuedouble > 20.0)
                                v->bpm = (float)jb->valuedouble;
                            cJSON_Delete(root);
                        }
                        v->q_cs = 0;            // fresh beat indices for QUANT
                        v->q_ln = 1 << 20;      // clamps down to the take length
                    }
                }
                v->pos = 0; v->rpos_i = 0;      // engine is parked (head_valid false)
                v->loading = false;
                if (v->autoplay) {              // fresh take: loop immediately —
                    v->autoplay = false;        // or, clock-synced, come in ON a
                    if (v->name[0]) {           // pulse at the in-phase offset
                        if (s3.rec_synced && (s3.ci.clk.locked || s3.int_bpm > 0.5f))
                            v->sync_start_req = true;
                        else v->playing = true;
                    }
                }
                worked = true;
                continue;
            }

            if (!rv[i].f) continue;

            if (v->window_req) {
                v->window_req = false;
                rebuild_head(v, &rv[i], stage);
                v->loading = false;
                worked = true;
                continue;
            }

            // (re)build the loop-start cache when the engine's wish moved —
            // ~0.5s of RAM at the window start so wraps of ANY window length
            // play instantly while the stream rewinds behind them
            {
                uint32_t lw = v->lsc_want;
                uint32_t d = lw > v->lsc_start ? lw - v->lsc_start
                                               : v->lsc_start - lw;
                if (v->lsc && lw && lw < v->play_len &&
                    (!v->lsc_valid || d > 2048)) {
                    v->lsc_valid = false;
                    uint32_t n = S3_LSC_FRAMES;
                    if (n > v->play_len - lw) n = v->play_len - lw;
                    uint32_t got = 0;
                    while (got < n) {
                        uint32_t want = n - got;
                        if (want > S3_CHUNK_FRAMES) want = S3_CHUNK_FRAMES;
                        uint32_t r = read_playback_frames(v, rv[i].f, &rv[i].sf, lw + got,
                                                          want, stage, stage);
                        if (r == 0) break;
                        memcpy(v->lsc + (size_t)got * 2, stage, r * 4);
                        got += r;
                        vTaskDelay(1);          // SD courtesy gap
                    }
                    v->lsc_start = lw;
                    v->lsc_frames = got;
                    v->lsc_valid = got > 0;
                    worked = true;
                }
            }

            if (v->retrig_req) {
                v->retrig_req = false;
                v->dbg_retrig++;
                // stream restarts at the seek target (crop starts beyond the
                // head land mid-file; plain retrigs land right after the head)
                uint32_t tgt = v->seek_frame;
                if (tgt < v->head_frames) tgt = v->head_frames;
                if (tgt > v->play_len) tgt = v->play_len;
                rv[i].stream_p = tgt;
                v->wpos = tgt;
                if (v->play_len <= v->head_frames) v->loading = false;  // RAM-resident
                worked = true;
            }

            // top up the ring: playback-order frames [head_frames..play_len).
            // Throttle bound is one chunk TIGHTER than the consumer's frame_ok
            // margin (RING - CHUNK): with both at the same bound, a full ring
            // put the play cursor exactly on the reject line — cursor froze,
            // freezing the throttle, muting the voice for good ~3.9s into any
            // long sample (the "short gate"/cutout + runaway starve counts).
            // a crop-CV sweep can park the cursor BEYOND wpos; unsigned
            // subtraction then reads as "ring overfull" and freezes the fill
            // (the deadlock family again). Clamp: cursor ahead = no lead.
            uint32_t lead = (v->wpos > v->rpos_i) ? v->wpos - v->rpos_i : 0;
            // stream limit: the engine caps the fill just past a looping
            // crop window so the ring KEEPS the window (seamless wraps)
            uint32_t cap = v->stream_cap;
            uint32_t limit = (cap && cap < v->play_len) ? cap : v->play_len;
            uint32_t want = (rv[i].stream_p < limit) ? limit - rv[i].stream_p : 0;
            if (want > S3_CHUNK_FRAMES) want = S3_CHUNK_FRAMES;
            // no dribble reads: sub-chunk toppers are only worth the SD
            // round-trip when they FINISH the file — a moving cap edge is
            // already covered by its 2-chunk margin
            if (want && want < 1024 && rv[i].stream_p + want < v->play_len)
                want = 0;
            if (want && lead < S3_RING_FRAMES - 2 * S3_CHUNK_FRAMES) {
                uint32_t got = read_playback_frames(v, rv[i].f, &rv[i].sf, rv[i].stream_p, want, stage, stage);
                if (got > 0) {
                    uint32_t w = rv[i].stream_p % S3_RING_FRAMES;
                    uint32_t first = S3_RING_FRAMES - w;
                    if (first > got) first = got;
                    memcpy(v->ring + w * 2, stage, first * 4);
                    if (first < got) memcpy(v->ring, stage + first * 2, (got - first) * 4);
                    rv[i].stream_p += got;
                    v->wpos = rv[i].stream_p;
                    worked = true;
                }
                uint32_t buffered = (v->wpos > v->rpos_i) ? v->wpos - v->rpos_i : 0;
                if (v->loading && (buffered >= S3_LOW_WATER || v->wpos + 1024 >= limit))
                    v->loading = false;
            } else if (v->loading && v->wpos + 1024 >= limit) {
                // a windowed refill smaller than LOW_WATER still completes;
                // "within the dribble margin of the cap" counts as done
                v->loading = false;
            }
        }

        if (!worked) vTaskDelay(1);   // >=1 tick: pdMS_TO_TICKS(5)==0 at 100Hz = busy-spin
    }
    for (int i = 0; i < S3_NVOICES; i++)
        if (rv[i].f) { sd_lock_take(); fclose(rv[i].f); sd_lock_give(); }
    free(stage);
    s_alive = false;
    vTaskDelete(NULL);
}

// ---- UI-side controls ---------------------------------------------------------

void s3_load_sample(int vid, const char *name)
{
    if (vid < 0 || vid >= S3_NVOICES) return;
    s3_voice_t *v = &s3.v[vid];
    v->playing = false;
    v->loading = true;
    strlcpy(v->pending, name, S3_NAME_LEN);
    v->load_req = true;
}

void s3_set_reverse(int vid, bool reverse)
{
    if (vid < 0 || vid >= S3_NVOICES) return;
    s3_voice_t *v = &s3.v[vid];
    if (v->reverse == reverse) return;
    v->reverse = reverse;
    if (v->name[0]) {
        v->loading = true;
        v->window_req = true;      // reader rebuilds head + stream (reversed)
    }
}

void s3_toggle_arm(int vid)
{
    if (vid < 0 || vid >= S3_NVOICES) return;
    if (recording_get_trig_func(vid) == TRIG_FUNC_RECORD) {
        recording_set_trig_func(vid, TRIG_FUNC_VOICE);
        s3.arm_target = -1;
        s3.rec_wait_vid = -1;
        recording_cancel_prepared();     // parked writer deletes its empty file
    } else {
        recording_set_trig_func(1 - vid, TRIG_FUNC_VOICE);   // one armed voice max
        recording_set_trig_func(vid, TRIG_FUNC_RECORD);
        s3.arm_target = vid;
        // pre-open the take's file now so the actual start (recording_trigger,
        // fired ON a clock pulse from the audio task) costs nothing
        recording_prepare(vid);
    }
}

// the dated 512-entry browser walk now lives in util/sample_ram
// (sample_list_recent) — the deck browses the same library the same way
_Static_assert(S3_NAME_LEN == 24, "sample_list_recent hands back char[24] ids");

int s3_list_samples(char (**names)[S3_NAME_LEN])
{
    return sample_list_recent(names);
}

// ---- engine --------------------------------------------------------------------

static esp_err_t s3_start(void)
{
    memset(&s3, 0, sizeof(s3));
    s3.monitor = true;
    s3.arm_mutes = true;     // sampler2 inheritance: arm = mute track, cue input
    s3.arm_target = -1;
    for (int i = 0; i < S3_NVOICES; i++) {
        s3_voice_t *v = &s3.v[i];
        v->head = heap_caps_malloc((size_t)S3_HEAD_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        v->ring = heap_caps_malloc((size_t)S3_RING_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        // loop-start cache is OPTIONAL: without it, long-window wraps just
        // keep the refill gap instead of failing the machine
        v->lsc = heap_caps_malloc((size_t)S3_LSC_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!v->head || !v->ring) {
            ESP_LOGE(TAG, "PSRAM alloc failed (voice %d)", i);
            for (int k = 0; k <= i; k++) { free(s3.v[k].head); free(s3.v[k].ring); free(s3.v[k].lsc); }
            return ESP_ERR_NO_MEM;
        }
        v->level = 1.0f;
        v->crop_start = 0;
        v->crop_len = 1.0f;
        v->ui_cs = 0;
        v->ui_ce = 1.0f;
        v->src_speed = 5 + i;            // CV6/CV7: speed-on-knob is how this
        v->src_start = -1;               // machine works (the good knobs)
        v->src_len = -1;
        v->crop_mode = S3_CROP_FREE;
        v->cs_sm = 0;
        v->ln_sm = 1.0f;
        v->ui_cs_min = 1.0f;             // jitter meter: extremes converge in
        v->ui_cs_max = 0;
        v->q_cs = 0;
        v->q_ln = 1 << 20;               // clamps down to the take length
        v->playmode = S3_MODE_LOOP;      // preset feel (Arlo): loops by default
        s3.cv12_floor[i] = 4095;         // converge down on first reads
    }
    s3.clk_src = 7;                      // CV8, same convention as deck/glitch
    s3.rec_wait_vid = -1;
    clockin_reset(&s3.ci, 4.0f);         // ppq restored by preset load
    s_run = true;
    // unpinned: file-reading tasks pinned to core 0 cause WiFi audio clicks
    xTaskCreate(reader_task, "s3_reader", 4096, NULL, 6, NULL);
    audio_status_set_voices("s3", "");
    return ESP_OK;
}

static void s3_stop(void)
{
    s_run = false;
    for (int i = 0; i < 100 && s_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < S3_NVOICES; i++) {
        free(s3.v[i].head); s3.v[i].head = NULL;
        free(s3.v[i].ring); s3.v[i].ring = NULL;
        free(s3.v[i].lsc);  s3.v[i].lsc = NULL;
    }
}

// does the loop-start cache cover playback-order frame f?
static inline bool s3_lsc_has(const s3_voice_t *v, uint32_t f)
{
    return v->lsc_valid && f >= v->lsc_start &&
           f - v->lsc_start < v->lsc_frames;
}

// fetch playback-order frame p (caller guarantees validity)
static inline void s3_fetch(const s3_voice_t *v, uint32_t p, float *l, float *r)
{
    const int16_t *s;
    if (p < v->head_frames)      s = v->head + (size_t)p * 2;
    else if (s3_lsc_has(v, p))   s = v->lsc + (size_t)(p - v->lsc_start) * 2;
    else                         s = v->ring + (size_t)(p % S3_RING_FRAMES) * 2;
    *l = (float)s[0];
    *r = (float)s[1];
}

// is playback-order frame f readable right now? Head frames always; ring
// frames only while streamed-and-not-yet-overwritten (a chunk of margin so
// a concurrent reader write can never clip the frame being read). Resident
// windows skip the margin — their ring slots are written once, never reused.
static inline bool s3_frame_ok(const s3_voice_t *v, uint32_t f, bool resident)
{
    if (f < v->head_frames) return true;
    if (s3_lsc_has(v, f)) return true;     // loop-start cache: always readable
    if (v->loading) return false;
    if (f >= v->wpos) return false;
    if (resident) return true;
    return (v->wpos - f) <= (S3_RING_FRAMES - S3_CHUNK_FRAMES);
}

// selector-cell adoption with hysteresis: unit-wide cells, a switch needs
// the continuous position 15% past the boundary — residual CV wobble can't
// flutter a quantized selection between neighbors
static inline int s3_cell_adopt(float pos, int cur)
{
    int li = (int)pos;
    if (li == cur) return cur;
    if (li > cur + 1 || li < cur - 1) return li;   // a real jump: no debate
    float frac = pos - (float)li;
    if (li > cur ? (frac > 0.15f) : (frac < 0.85f)) return li;
    return cur;
}

// matrix CV read — from the MEDIAN-conditioned snapshot (WiFi-burst ADC
// spikes of ±80 counts punched through slew + hysteresis: jumpy crop
// points). ch1/2 (1V/oct jacks) idle ~21% up the scale by analog design —
// rescale from the tracked floor so a patched mod source spans the full
// 0..4095 range instead of starting a fifth of the way up.
static inline int s3_mod_read(const machine_io_t *io, int src)
{
    (void)io;
    int c = s3.cv_med[src & 7];
    if ((src & 7) < 2) {
        int fl = s3.cv12_floor[src & 7];
        if (fl > 3800) return 0;              // tracker not converged / dead ch
        c = (int)((int32_t)(c - fl) * 4095 / (4095 - fl));
        if (c < 0) c = 0;
        if (c > 4095) c = 4095;
    }
    return c;
}

static void s3_process(int32_t out[MACHINE_BLOCK],
                       const int32_t in[MACHINE_BLOCK],
                       const machine_io_t *io)
{
    static uint8_t prev_trig = 0x03;      // seed idle-high: no phantom edge
    const int frames = MACHINE_BLOCK / 2;

    uint8_t fell = prev_trig & (~io->trig_level) & 0x03;
    prev_trig = io->trig_level;

    // ch1/2 floor trackers: dips follow instantly, drift back up slowly
    // (the deck clk_base pattern)
    for (int c = 0; c < 2; c++) {
        int cv = io->cv[c];
        if (cv < s3.cv12_floor[c]) s3.cv12_floor[c] = cv;
        else if (s3.cv12_floor[c] < 4095) s3.cv12_floor[c]++;
    }

    // median-of-5 CV conditioning: one snapshot per block for all matrix
    // reads. Impulse spikes (WiFi bursts) vanish; sustained knob moves lag
    // by two blocks (~3 ms) — imperceptible.
    for (int c = 0; c < 8; c++) {
        s3.cv_hist[c][s3.cv_hp] = io->cv[c];
        uint16_t m[5];
        memcpy(m, s3.cv_hist[c], sizeof(m));
        for (int a = 0; a < 4; a++)              // tiny insertion sort
            for (int b = a + 1; b < 5; b++)
                if (m[b] < m[a]) { uint16_t t = m[a]; m[a] = m[b]; m[b] = t; }
        s3.cv_med[c] = m[2];
    }
    s3.cv_hp = (s3.cv_hp + 1) % 5;

    // Gate workflow (Arlo, 2026-07-12): press triggers the voice as usual;
    // HOLD ~1 s while idle = arm the track. Armed: press = start recording,
    // press again = stop + auto-load + disarm. Holding through a take you
    // just started = abort it (stopped, discarded, disarmed). The falling
    // edge always acts instantly — the arm hold can't hide that first
    // trigger without adding latency to every normal gate.
    static uint32_t hold[S3_NVOICES];         // frames the gate has been low
    static bool hold_fired[S3_NVOICES];
    static bool rec_started_this_press[S3_NVOICES];
    static uint32_t since_fell[S3_NVOICES];   // frames since the PREVIOUS press
    static uint32_t prev_gap[S3_NVOICES] = {(uint32_t)1 << 30, (uint32_t)1 << 30};
    static bool rapid_press[S3_NVOICES];      // press arrived mid-SEQUENCE
    const uint32_t HOLD_ARM = S3_RATE;        // ~1 s
    // sequencer guard: only a press with TWO OR MORE predecessors inside this
    // window is "mid-sequence" and can never arm. A single recent press (you
    // triggered, now you hold to arm — the live move) stays armable; sustained
    // gate streams (a sequencer's long repeating notes) stay locked out.
    const uint32_t RAPID_WIN = (uint32_t)(2.5f * S3_RATE);

    for (int i = 0; i < S3_NVOICES; i++) {
        s3_voice_t *v = &s3.v[i];
        bool low = !(io->trig_level & (1 << i));
        if (since_fell[i] < RAPID_WIN) since_fell[i] += (uint32_t)frames;
        if (fell & (1 << i)) {
            rapid_press[i] = since_fell[i] < RAPID_WIN && prev_gap[i] < RAPID_WIN;
            prev_gap[i] = since_fell[i];
            since_fell[i] = 0;
            hold[i] = (uint32_t)frames;
            hold_fired[i] = false;
            rec_started_this_press[i] = false;
            if (recording_get_trig_func(i) == TRIG_FUNC_RECORD) {
                // armed: the gate drives the take instead of the voice. With
                // a locked clock, start lands ON the next pulse and stop on
                // the next whole beat (loop-ready lengths); free-run acts now.
                // trigger/finish are bare atomics — audio-task safe.
                bool clk_ok = s3.ci.clk.locked || s3.int_bpm > 0.5f;
                if (!recording_is_active()) {
                    rec_started_this_press[i] = true;
                    if (clk_ok) {
                        s3.rec_wait_vid = i;          // start on the next pulse
                    } else {
                        recording_trigger();
                        s3.rec_synced = false;
                        s3.rec_frames = 0;
                        s3.rec_pulses = 0;
                        s3.rec_first_pulse = 0;
                    }
                } else {
                    if (clk_ok) s3.rec_stop_wait = true;   // stop on the beat
                    else recording_finish();
                }
            } else if (v->name[0] && v->head_valid) {
                if (v->playing) {
                    // gate toggles: press while playing = pause (also makes the
                    // arm-hold graceful — its initial press quiets the track
                    // instead of blasting a retrigger)
                    v->playing = false;
                } else {
                    // trigger from the crop start (last block's effective crop;
                    // instant when it's in the head, a resident sample, OR
                    // still sitting in the ring's trailing window)
                    uint32_t cs_f = (uint32_t)(v->ui_cs * (float)v->play_len);
                    v->pos = cs_f;
                    v->playing = true;
                    bool resident = (v->play_len <= v->head_frames + S3_RING_FRAMES) &&
                                    v->wpos >= v->play_len;
                    if (v->play_len > v->head_frames &&
                        !s3_frame_ok(v, cs_f, resident)) {
                        v->loading = true;     // parks ring reads until refilled
                        v->seek_frame = cs_f > S3_XFADE_FRAMES + 2048
                                     ? cs_f - S3_XFADE_FRAMES - 2048 : 0;
                                       // pre-roll included; reader clamps
                        v->retrig_req = true;
                    }
                }
            }
        } else if (low && hold[i] > 0) {
            hold[i] += (uint32_t)frames;
            if (!hold_fired[i] && hold[i] >= HOLD_ARM) {
                hold_fired[i] = true;
                bool other_low = !(io->trig_level & (1 << (1 - i)));
                if (recording_is_active() && rec_started_this_press[i]) {
                    s_rec_abort_req = true;    // held through the take: abort it
                } else if (!recording_is_active() && !rapid_press[i] &&
                           !other_low) {       // both-held = the JOINT gesture
                    s_arm_vid = i;             // idle hold: arm (or disarm) this track
                    s_arm_req = true;
                }
            }
        } else if (!low) {
            hold[i] = 0;
        }
    }

    // BOTH gates held ~1s with both tracks loaded = JOINT GRID SNAP (Arlo):
    // the two loops restart from their window starts ON the same clock
    // pulse — mutually locked and back on the grid in one gesture. The
    // initial presses paused both (press-while-playing), so they drop out
    // together and re-enter together.
    {
        static bool both_fired = false;
        bool both_low = (io->trig_level & 0x03) == 0;
        if (both_low && !both_fired &&
            hold[0] >= HOLD_ARM && hold[1] >= HOLD_ARM &&
            s3.v[0].name[0] && s3.v[1].name[0] && !recording_is_active()) {
            both_fired = true;
            s3.v[0].sync_snap_req = true;
            s3.v[1].sync_snap_req = true;
        }
        if (!both_low) both_fired = false;
    }

    bool rec_active = recording_is_active();
    int  rec_vid = recording_get_target_vid();
    bool armed = (recording_get_trig_func(0) == TRIG_FUNC_RECORD) ||
                 (recording_get_trig_func(1) == TRIG_FUNC_RECORD);
    bool pass_input = rec_active || (armed && s3.monitor);

    float mix_l[MACHINE_BLOCK / 2], mix_r[MACHINE_BLOCK / 2];
    for (int fno = 0; fno < frames; fno++) {
        if (pass_input) {
            mix_l[fno] = (float)(in[fno * 2]     >> 16);
            mix_r[fno] = (float)(in[fno * 2 + 1] >> 16);
        } else {
            mix_l[fno] = 0;
            mix_r[fno] = 0;
        }
    }

    for (int i = 0; i < S3_NVOICES; i++) {
        s3_voice_t *v = &s3.v[i];
        // recording target is always muted; an ARMED track mutes too when
        // arm_mutes is on (sampler2 behavior: arm = cue the input instead)
        bool muted = (rec_active && rec_vid == i) ||
                     (s3.arm_mutes && recording_get_trig_func(i) == TRIG_FUNC_RECORD);

        // rate = through-zero speed from the assigned CV (matrix source).
        // Speed curve (Arlo): knob center = unity; CW half sweeps to +150%;
        // CCW half sweeps down THROUGH ZERO into reverse, clamped at -100%.
        // Plateaus at unity and zero make both dependable knob targets.
        float rate = 1.0f;
        if (v->src_speed >= 0) {
            int pc = s3_mod_read(io, v->src_speed);
            // sticky unity: a wide dead zone in knob counts around centre,
            // each side rescaled from the plateau edge (no value jump)
            const int DZ = 180;
            if (pc >= 2048 - DZ && pc <= 2048 + DZ)
                rate = 1.0f;
            else if (pc > 2048 + DZ)
                rate = 1.0f + 0.5f * (float)(pc - 2048 - DZ) / (float)(2047 - DZ);
            else
                rate = -1.0f + 2.0f * (float)pc / (float)(2048 - DZ);
            if (rate > -0.04f && rate < 0.04f) rate = 0.0f;   // dependable stop
        }
        v->cur_rate = rate;                     // UI: native-speed badge

        // effective CROP this block: stored params, overridden live by any
        // assigned CV (performable — pure math, nothing touches the reader).
        // Sampler2 semantics: START + LENGTH. Start slides the WHOLE window,
        // length rides along; at EOF the length gives way (and comes back as
        // start retreats). Speed/Start/Length each have their own CV source.
        float cs = 0, ln = 1.0f;
        if (v->crop_mode != S3_CROP_OFF) {
            cs = v->crop_start;
            ln = v->crop_len;
            if (v->src_start >= 0) cs = (float)s3_mod_read(io, v->src_start) / 4095.0f * 0.98f;
            if (v->src_len >= 0)   ln = 0.02f + (float)s3_mod_read(io, v->src_len) / 4095.0f * 0.98f;
        }
        // slew: raw ADC noise jitters the window by 10s of ms on long takes —
        // jumpy crop shading, and the start noise kept re-snapping the
        // playhead ("plays a couple clicks and starts over")
        v->cs_sm += 0.05f * (cs - v->cs_sm);
        v->ln_sm += 0.05f * (ln - v->ln_sm);
        cs = v->cs_sm;
        ln = v->ln_sm;
        float ce;
        // QUANT2: the musical ladder — length picks from 1/4, 1/2, 1, 2, 4,
        // 8, 16, 32 beats (whatever fits the take). START anchors to the
        // whole-beat grid INDEPENDENT of length: growing the loop extends
        // the TAIL only (deriving start from length-sized slots yanked the
        // start backward whenever the length stepped up — Arlo).
        if (v->crop_mode == S3_CROP_QUANT2 && v->bpm > 20.0f && v->play_len) {
            float bf = 60.0f * 44100.0f / v->bpm;        // frames per beat
            float nb = (float)v->play_len / bf;          // beats in the take
            float nb4 = 4.0f * nb;                       // quarter-beats
            if (nb4 >= 1.0f) {
                int nb4i = (int)nb4;
                int steps = 1;                           // ladder 2^0..2^(steps-1)
                while ((1 << steps) <= nb4i && steps < 9) steps++;
                // LENGTH control = selector across the ladder (cell hysteresis)
                float lpos = ln * (float)steps;
                v->q_ln = s3_cell_adopt(lpos, v->q_ln);
                if (v->q_ln < 0) v->q_ln = 0;
                if (v->q_ln > steps - 1) v->q_ln = steps - 1;
                int lq = 1 << v->q_ln;                   // window, quarter-beats
                // START control = whole-beat anchor (length never moves it)
                int nbi = (int)nb;
                if (nbi < 1) nbi = 1;
                float cb = (cs / 0.98f) * (float)nb;     // continuous beat pos
                v->q_cs = s3_cell_adopt(cb, v->q_cs);
                if (v->q_cs < 0) v->q_cs = 0;
                if (v->q_cs > nbi - 1) v->q_cs = nbi - 1;
                cs = (float)v->q_cs / nb;
                ln = (float)lq / nb4;
                // end = start + length, giving way at EOF (generic clamps below)
            }
        }
        // tempo-QUANTIZED crop: start and length snap to whole beats of the
        // take's stamped tempo. Adopted indices move only when the continuous
        // value clearly leaves the current beat (0.6-beat hysteresis).
        if (v->crop_mode == S3_CROP_QUANT && v->bpm > 20.0f && v->play_len) {
            float bf = 60.0f * 44100.0f / v->bpm;        // frames per beat
            float nb = (float)v->play_len / bf;          // beats in the take
            if (nb >= 2.0f) {
                int nbi = (int)(nb + 0.5f);
                float cb = cs * nb, lb = ln * nb;        // continuous beats
                if (cb - (float)v->q_cs > 0.6f || (float)v->q_cs - cb > 0.6f)
                    v->q_cs = (int)(cb + 0.5f);
                if (lb - (float)v->q_ln > 0.6f || (float)v->q_ln - lb > 0.6f)
                    v->q_ln = (int)(lb + 0.5f);
                if (v->q_cs < 0) v->q_cs = 0;
                if (v->q_cs > nbi - 1) v->q_cs = nbi - 1;
                if (v->q_ln < 1) v->q_ln = 1;
                if (v->q_ln > nbi) v->q_ln = nbi;
                cs = (float)v->q_cs / nb;
                ln = (float)v->q_ln / nb;
            }
        }
        ce = cs + ln;
        if (ce > 1.0f) ce = 1.0f;               // length gives way at EOF
        if (cs > ce - 0.02f) cs = ce - 0.02f;
        if (cs < 0) cs = 0;
        v->ui_cs = cs;                          // UI shades from these
        v->ui_ce = ce;
        if (cs < v->ui_cs_min) v->ui_cs_min = cs;   // jitter meter extremes
        if (cs > v->ui_cs_max) v->ui_cs_max = cs;
        // windowed streaming: while LOOPing, cap the stream just past the
        // window — the reader must not race to EOF and evict the very window
        // from the ring (that forced a seek+mute at every wrap). Full-file
        // windows and small (residency-bound) samples stream uncapped.
        // CHUNK-quantized: the slewed window edge creeps a few frames per
        // block, and a raw cap turned that into thousands of dribble SD
        // reads per second (SD-bus thrash = mid-playback clicks + starved
        // WiFi/httpd tasks).
        if (v->play_len > v->head_frames + S3_RING_FRAMES &&
            v->playmode == S3_MODE_LOOP) {
            uint32_t cap = (uint32_t)(ce * (float)v->play_len) + 2 * S3_CHUNK_FRAMES;
            cap = ((cap + S3_CHUNK_FRAMES - 1) / S3_CHUNK_FRAMES) * S3_CHUNK_FRAMES;
            v->stream_cap = cap;
        } else {
            v->stream_cap = 0;
        }
        uint32_t cs_f = (uint32_t)(cs * (float)v->play_len);
        uint32_t ce_f = (uint32_t)(ce * (float)v->play_len);
        if (ce_f > v->play_len) ce_f = v->play_len;
        if (ce_f < cs_f + 64) ce_f = (cs_f + 64 <= v->play_len) ? cs_f + 64 : v->play_len;

        float lg = (v->pan <= 0) ? 1.0f : 1.0f - v->pan;
        float rg = (v->pan >= 0) ? 1.0f : 1.0f + v->pan;
        lg *= v->level;
        rg *= v->level;

        // whole window in RAM (head + one linear ring pass, never overwritten)
        // -> reverse/bidirectional play is free. Streamed samples can reverse
        // only through the still-buffered window behind the stream head.
        bool resident = (v->play_len <= v->head_frames + S3_RING_FRAMES) &&
                        v->wpos >= v->play_len;

        // window-motion detector: while a fast start sweep is in flight, an
        // unbuffered snap must NOT seek every block (seek + 0.25s refill +
        // window moved on = one long mute for the whole sweep). Hold off;
        // the landing fires one clean seek. Slow sweeps (LFO) stay "still".
        {
            uint32_t d = (cs_f > v->last_cs_f) ? cs_f - v->last_cs_f
                                               : v->last_cs_f - cs_f;
            v->last_cs_f = cs_f;
            if (d > (v->play_len >> 10)) v->cs_moving = 24;    // ~35 ms hold
            else if (v->cs_moving) v->cs_moving--;
        }

        // loop-start cache wish: a streamed LOOP whose start lies beyond the
        // head wants ~0.5s of RAM starting one crossfade BEFORE the window
        // start (the fade blends in the pre-roll) — wraps then play from RAM
        // at ANY window length. Published only while the start is stable;
        // the head already covers early starts.
        v->lsc_want = (v->playmode == S3_MODE_LOOP && !v->cs_moving &&
                       v->play_len > v->head_frames + S3_RING_FRAMES &&
                       cs_f >= v->head_frames)
                      ? cs_f - S3_XFADE_FRAMES : 0;

        // loop-seam crossfade length. PRE-ROLL construction: the last xf
        // frames of the window blend with the content BEFORE the window
        // start, and the wrap lands exactly ON the start — the loop period
        // stays exactly W. (Blending the start itself in and landing after
        // it SHORTENED every pass by xf: loops drifted early against the
        // monitored source — Arlo.) No pre-roll before the file start, so
        // full-take loops run unfaded (they're circular: their "pre-roll"
        // IS the tail).
        uint32_t xf = S3_XFADE_FRAMES;
        if (ce_f > cs_f && xf > (ce_f - cs_f) / 4) xf = (ce_f - cs_f) / 4;
        if (xf > cs_f) xf = cs_f;
        if (xf < 8) xf = 0;

        // SEAM LATCH: the fade and the wrap must share ONE window geometry.
        // FREE-mode CVs move the corners a few frames every block, and the
        // fade zone spans ~16 blocks — mid-fade geometry shifts made the
        // incoming pre-roll leg jump between blocks (occasional seam click).
        // Latch on entering the fade zone; the wrap releases it, so live
        // edits simply land on the next pass.
        if (v->seam_on) {
            if (!v->playing || rate <= 0 || v->playmode != S3_MODE_LOOP ||
                v->pos < (double)v->seam_ce - 4.0 * (double)(v->seam_xf ? v->seam_xf : 64))
                v->seam_on = false;              // context gone / end swept away
        }
        if (!v->seam_on && v->playing && rate > 0 &&
            v->playmode == S3_MODE_LOOP && xf &&
            v->pos >= (double)ce_f - (double)xf - (double)frames * 3.0 &&
            v->pos < (double)ce_f) {
            v->seam_cs = cs_f;
            v->seam_ce = ce_f;
            v->seam_xf = xf;
            v->seam_on = true;
        }
        const uint32_t f_cs = v->seam_on ? v->seam_cs : cs_f;
        const uint32_t f_ce = v->seam_on ? v->seam_ce : ce_f;
        const uint32_t f_xf = v->seam_on ? v->seam_xf : xf;

        bool starved = false;
        for (int fno = 0; fno < frames; fno++) {
            // CROP edges first: forward end wraps (loop) or stops (one-shot);
            // reverse start wraps-to-end only when the whole window is resident.
            // KEY: a wrap/snap whose target is still in the ring (it holds ~4s
            // behind the stream head) is pure cursor math — NO seek, NO gap.
            // Crop windows within ring reach loop and slide seamlessly.
            if (v->playing && v->head_valid) {
                if (rate >= 0 && v->pos >= (double)f_ce - 1.0) {
                    if (v->playmode == S3_MODE_LOOP) {
                        if (v->play_len > v->head_frames &&
                            !s3_frame_ok(v, f_cs, resident)) {
                            v->pos = (double)f_cs;
                            v->loading = true;
                            v->seek_frame = f_cs > S3_XFADE_FRAMES + 2048
                                          ? f_cs - S3_XFADE_FRAMES - 2048 : 0;
                            v->retrig_req = true;
                        } else {
                            // PHASE-EXACT wrap: subtract the window. The
                            // fade already blended toward the pre-roll, so
                            // this lands seamlessly at/just before cs and
                            // the loop period is exactly W — no drift
                            // against a synced source.
                            v->pos -= (double)(f_ce - f_cs);
                            // PREFETCH: wrap landed in cheap RAM (head or
                            // loop-start cache) but the ring beyond it is
                            // stale (still holds the pre-wrap tail) —
                            // rewind the stream NOW, while RAM plays, so
                            // the handoff at the cache edge is seamless.
                            // No `loading`: audio never stops.
                            uint32_t cov = 0;      // RAM coverage from f_cs
                            if (f_cs < v->head_frames) cov = v->head_frames;
                            if (s3_lsc_has(v, f_cs)) {
                                uint32_t le = v->lsc_start + v->lsc_frames;
                                if (le > cov) cov = le;
                            }
                            if (v->play_len > v->head_frames && cov &&
                                cov < v->play_len &&
                                f_cs + 2048 < cov &&
                                !s3_frame_ok(v, cov, resident) &&
                                !v->retrig_req) {
                                v->seek_frame = cov;
                                v->retrig_req = true;
                            }
                        }
                        v->seam_on = false;        // seam complete: live geometry
                    } else {
                        v->playing = false;
                    }
                } else if (rate < 0 && v->pos <= (double)cs_f) {
                    if (v->playmode == S3_MODE_LOOP && resident)
                        v->pos = (double)ce_f - 1.001;
                    // else: park at the crop start till the knob comes back
                }
                // crop start swept above the cursor (live performance): snap
                // in — but only past a 1%-of-take hysteresis band, so residual
                // point wobble can't hold the playhead hostage at the start
                if (v->playmode == S3_MODE_LOOP && rate > 0 &&
                    v->pos < (double)cs_f - (double)v->play_len / 100.0) {
                    if (s3_frame_ok(v, cs_f, resident)) {
                        v->pos = (double)cs_f;             // buffered: instant
                    } else if (!v->cs_moving && v->play_len > v->head_frames) {
                        v->pos = (double)cs_f;             // landed: one seek
                        v->loading = true;
                        v->seek_frame = cs_f > S3_XFADE_FRAMES + 2048
                                          ? cs_f - S3_XFADE_FRAMES - 2048 : 0;
                        v->retrig_req = true;
                    }
                    // else: sweep in flight over unbuffered ground — keep
                    // playing what we have; the landing snaps us in
                }
            }
            uint32_t p = (uint32_t)(v->pos < 0 ? 0 : v->pos);
            bool have = s3_frame_ok(v, p, resident) && s3_frame_ok(v, p + 1, resident) &&
                        (p + 1) < f_ce;
            bool can_play = v->playing && !muted && v->head_valid &&
                            rate != 0.0f && have;
            if (v->playing && !muted && v->head_valid && !have && !v->loading &&
                (p + 1) < f_ce)
                starved = true;
            float gt = can_play ? 1.0f : 0.0f;
            v->out_gain += (gt - v->out_gain) * 0.015f;
            if (!can_play) {
                v->last_l *= 0.94f;
                v->last_r *= 0.94f;
                mix_l[fno] += v->last_l;
                mix_r[fno] += v->last_r;
                continue;
            }
            float l0, r0, l1, r1;
            s3_fetch(v, p, &l0, &r0);
            s3_fetch(v, p + 1, &l1, &r1);
            float fr = (float)(v->pos - (double)p);
            float lraw = l0 + (l1 - l0) * fr;
            float rraw = r0 + (r1 - r0) * fr;
            // loop-seam crossfade: inside the fade zone blend in the
            // PRE-ROLL (content before the window start), offset so the
            // phase-exact wrap lands in perfect continuity: at p = ce-1 the
            // incoming sits at cs-1, and the next frame is cs.
            if (v->playmode == S3_MODE_LOOP && rate > 0 && f_xf &&
                p >= f_ce - f_xf && p < f_ce) {
                uint32_t p2 = f_cs - (f_ce - p);
                if (s3_frame_ok(v, p2, resident) &&
                    s3_frame_ok(v, p2 + 1, resident)) {
                    float m0, n0, m1, n1;
                    s3_fetch(v, p2, &m0, &n0);
                    s3_fetch(v, p2 + 1, &m1, &n1);
                    float k = (float)(f_ce - p) / (float)f_xf; // 1 -> 0
                    // equal-power: at 23ms a linear blend audibly dips
                    // mid-fade on uncorrelated material
                    float ko = sqrtf(k), ki = sqrtf(1.0f - k);
                    lraw = lraw * ko + (m0 + (m1 - m0) * fr) * ki;
                    rraw = rraw * ko + (n0 + (n1 - n0) * fr) * ki;
                }
            }
            float l = lraw * lg * v->out_gain;
            float r = rraw * rg * v->out_gain;
            v->last_l = l;
            v->last_r = r;
            mix_l[fno] += l;
            mix_r[fno] += r;
            v->pos += rate;
            if (rate < 0 && v->pos < (double)cs_f) v->pos = (double)cs_f;
            if (v->pos < 0) v->pos = 0;
        }
        if (starved) v->dbg_starve++;
        // self-heal a teleported cursor. Sampler2 kept the playhead inside
        // the crop zone by snapping it back; here the STREAM must follow the
        // snap too — a crop-CV sweep that lands the cursor outside the
        // buffered window otherwise starves the voice forever (no loop wrap
        // ever fires the seek, and the reader's lead froze — the deadlock
        // family). One shot: loading parks the engine until the refill lands.
        if (starved && !v->loading && !v->retrig_req &&
            v->play_len > v->head_frames) {
            uint32_t p = (uint32_t)(v->pos < 0 ? 0 : v->pos);
            // the interpolator needs p AND p+1 — gate on the frame that's
            // actually MISSING. (p parked on the LAST head frame with a
            // stale ring wedged forever: p itself was fine, p+1 wasn't,
            // and a p-based gate never fired. Off by one, 189k starves.)
            uint32_t miss = !s3_frame_ok(v, p, resident) ? p : p + 1;
            bool ahead  = miss >= v->wpos + S3_CHUNK_FRAMES;   // teleport, not a crawl
            bool behind = v->wpos > miss &&
                          (v->wpos - miss) > (S3_RING_FRAMES - S3_CHUNK_FRAMES);
            if (miss >= v->head_frames && (ahead || behind)) {
                v->loading = true;
                v->seek_frame = p;          // reader clamps up to head_frames
                v->retrig_req = true;
                v->dbg_heal++;
            }
            // sledgehammer: ANY starve that persists ~150ms is a wedge some
            // gate above failed to classify — seek to the cursor regardless
            v->starve_run++;
            if (v->starve_run > 100 && !v->retrig_req) {
                v->loading = true;
                v->seek_frame = p;
                v->retrig_req = true;
                v->starve_run = 0;
                v->dbg_heal++;
            }
        } else if (!starved) {
            v->starve_run = 0;
        }
        // per-block integer mirror for the reader's throttle + the UI bar
        v->rpos_i = (uint32_t)(v->pos < 0 ? 0 : v->pos);
    }

    for (int fno = 0; fno < frames; fno++) {
        float l = mix_l[fno], r = mix_r[fno];
        if (l > 32767.0f) l = 32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r > 32767.0f) r = 32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        out[fno * 2]     = ((int32_t)l) << 16;
        out[fno * 2 + 1] = ((int32_t)r) << 16;
    }

    // ---- CV clock: the shared conditioned front-end (Schmitt + floor +
    // detector + ghost gate), pulses-per-beat carried in the service --------
    bool edge = clockin_block(&s3.ci, io->cv[s3.clk_src & 7], frames);

    // internal clock: when no external clock is locked (external always wins),
    // a settable-BPM metronome supplies the sync pulses — the whole synced
    // workflow (quantized takes, tempo stamp, in-phase re-entry) works
    // standalone. Same pulses-per-beat as the external setting.
    bool ext_lock = s3.ci.clk.locked;
    bool int_on = !ext_lock && s3.int_bpm > 0.5f;
    bool clk_ok = ext_lock || int_on;
    if (int_on) {
        uint32_t ip = (uint32_t)(44100.0f * 60.0f / (s3.int_bpm * s3.ci.ppb));
        s3.int_since += (uint32_t)frames;
        if (s3.int_since >= ip) {
            s3.int_since -= ip;
            edge = true;                       // internal pulse
        }
    } else {
        s3.int_since = 0;
    }

    // ---- clock-synced capture bookkeeping (audio-task-owned) ----------------
    s3.post_stop_frames += (uint32_t)frames;   // free-runs; reset at synced stop
    if (recording_is_active()) {
        s3.rec_frames += (uint32_t)frames;
        if (edge) {
            if (s3.rec_pulses == 0 && !s3.rec_synced && s3.rec_first_pulse == 0)
                s3.rec_first_pulse = s3.rec_frames;    // grid marker, unsynced start
            s3.rec_pulses++;
            uint32_t ppw = (uint32_t)(s3.ci.ppb + 0.5f);       // pulses per beat
            if (ppw < 1) ppw = 1;
            if (s3.rec_stop_wait && (s3.rec_pulses % ppw) == 0) {
                recording_finish();                    // stop ON the whole beat
                s3.rec_stop_wait = false;
                s3.post_stop_frames = 0;               // phase ref for sync re-entry
                float bpm = ext_lock ? clockin_beat_bpm(&s3.ci)
                          : int_on ? s3.int_bpm : 0;
                if (bpm > 0) {
                    s3.rec_bpm = bpm;
                    s3.rec_stamp_req = true;           // reader amends the sidecar
                }
            }
        }
        if (s3.rec_stop_wait && !clk_ok) {
            recording_finish();                        // clock died mid-wait
            s3.rec_stop_wait = false;
        }
    }

    // synced re-entry: a fresh clock-synced take starts ON a pulse, its play
    // position offset by the frames elapsed since the stop edge — the loop
    // comes in exactly in phase, however long the save/load took. Position
    // maps into the CROP window (fresh takes default to the full sample).
    for (int i = 0; i < S3_NVOICES; i++) {
        s3_voice_t *v = &s3.v[i];
        if (!v->sync_start_req) continue;
        uint32_t cs_f = (uint32_t)(v->ui_cs * (float)v->play_len);
        uint32_t ce_f = (uint32_t)(v->ui_ce * (float)v->play_len);
        uint32_t L = (ce_f > cs_f) ? ce_f - cs_f : (v->play_len ? v->play_len : 1);
        if (!clk_ok) {                                 // clock gone: just start
            v->pos = (double)cs_f;
            v->playing = true;
            v->sync_start_req = false;
        } else if (edge && v->play_len) {
            v->pos = (double)(cs_f + (s3.post_stop_frames % L));
            v->playing = true;
            v->sync_start_req = false;
        }
    }

    // JOINT GRID SNAP: both voices consume the SAME pulse edge, restarting
    // from their window starts — together, and on the grid. No clock: land
    // immediately (still together, this same block).
    for (int i = 0; i < S3_NVOICES; i++) {
        s3_voice_t *v = &s3.v[i];
        if (!v->sync_snap_req) continue;
        if (!v->name[0] || !v->head_valid) { v->sync_snap_req = false; continue; }
        if (clk_ok && !edge) continue;
        uint32_t cs_f = (uint32_t)(v->ui_cs * (float)v->play_len);
        v->pos = (double)cs_f;
        v->playing = true;
        v->sync_snap_req = false;
        // streamed start not in RAM: seek (the loop-start cache usually has
        // it, since these loops were just playing)
        bool res = (v->play_len <= v->head_frames + S3_RING_FRAMES) &&
                   v->wpos >= v->play_len;
        if (v->play_len > v->head_frames && !s3_frame_ok(v, cs_f, res)) {
            v->loading = true;
            v->seek_frame = cs_f > S3_XFADE_FRAMES + 2048
                          ? cs_f - S3_XFADE_FRAMES - 2048 : 0;
            v->retrig_req = true;
        }
    }

    if (!recording_is_active() && s3.rec_wait_vid >= 0) {
        if (edge && clk_ok) {
            recording_trigger();                       // start ON the pulse:
            s3.rec_synced = true;                      // downbeat = frame 0
            s3.rec_frames = 0;
            s3.rec_pulses = 0;
            s3.rec_first_pulse = 0;
            s3.rec_wait_vid = -1;
        } else if (!clk_ok) {
            recording_trigger();                       // clock died: free-run now
            s3.rec_synced = false;
            s3.rec_frames = 0;
            s3.rec_pulses = 0;
            s3.rec_first_pulse = 0;
            s3.rec_wait_vid = -1;
        }
    }
}

// ---- preset ---------------------------------------------------------------------

static cJSON *s3_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "s3v", 2);       // schema version gate
    cJSON_AddBoolToObject(o, "monitor", s3.monitor);
    cJSON_AddBoolToObject(o, "arm_mutes", s3.arm_mutes);
    cJSON_AddNumberToObject(o, "clk_src", s3.clk_src);
    cJSON_AddNumberToObject(o, "int_bpm", s3.int_bpm);
    cJSON_AddNumberToObject(o, "ppq", s3.ci.ppb);
    cJSON *va = cJSON_CreateArray();
    cJSON_AddItemToObject(o, "voices", va);
    for (int i = 0; i < S3_NVOICES; i++) {
        s3_voice_t *v = &s3.v[i];
        cJSON *vo = cJSON_CreateObject();
        cJSON_AddStringToObject(vo, "name", v->name);
        cJSON_AddNumberToObject(vo, "mode", v->playmode);
        cJSON_AddBoolToObject(vo, "rev", v->reverse);
        cJSON_AddNumberToObject(vo, "cs", v->crop_start);
        cJSON_AddNumberToObject(vo, "cl", v->crop_len);
        cJSON_AddNumberToObject(vo, "m_sp", v->src_speed);
        cJSON_AddNumberToObject(vo, "m_st", v->src_start);
        cJSON_AddNumberToObject(vo, "m_ln", v->src_len);
        cJSON_AddNumberToObject(vo, "cm", v->crop_mode);
        cJSON_AddNumberToObject(vo, "level", v->level);
        cJSON_AddNumberToObject(vo, "pan", v->pan);
        cJSON_AddItemToArray(va, vo);
    }
    return o;
}

static void s3_preset_load(const cJSON *node)
{
    // version gate: anything but our own v2 schema loads DEFAULTS. The
    // sampler2 rename poisoning (old machine's blob under the new name ->
    // volume 0 on both voices, "silent sampler") is exactly what this stops.
    cJSON *j;
    if (!node || !(j = cJSON_GetObjectItemCaseSensitive(node, "s3v")) ||
        !cJSON_IsNumber(j) || j->valueint != 2) {
        if (node) ESP_LOGW(TAG, "autosave schema mismatch — using defaults");
        return;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "monitor"))) s3.monitor = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "arm_mutes"))) s3.arm_mutes = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "clk_src")) && cJSON_IsNumber(j))
        s3.clk_src = j->valueint & 7;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "int_bpm")) && cJSON_IsNumber(j)) {
        float b = (float)j->valuedouble;
        s3.int_bpm = (b >= 40.0f && b <= 240.0f) ? b : 0;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "ppq")) && cJSON_IsNumber(j)) {
        float q = (float)j->valuedouble;
        clockin_set_ppb(&s3.ci, (q == 1 || q == 2 || q == 4 || q == 8) ? q : 4.0f);
    }
    cJSON *va = cJSON_GetObjectItemCaseSensitive(node, "voices");
    for (int i = 0; i < S3_NVOICES; i++) {
        cJSON *vo = va ? cJSON_GetArrayItem(va, i) : NULL;
        if (!vo) continue;
        s3_voice_t *v = &s3.v[i];
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "mode")) && cJSON_IsNumber(j))
            v->playmode = j->valueint == S3_MODE_LOOP ? S3_MODE_LOOP : S3_MODE_ONESHOT;
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "rev"))) v->reverse = cJSON_IsTrue(j);
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "cs")) && cJSON_IsNumber(j)) {
            float s = (float)j->valuedouble;
            v->crop_start = (s < 0 || s > 0.98f) ? 0 : s;
        }
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "cl")) && cJSON_IsNumber(j)) {
            float l = (float)j->valuedouble;
            v->crop_len = (l < 0.02f || l > 1.0f) ? 1.0f : l;
        } else if ((j = cJSON_GetObjectItemCaseSensitive(vo, "ce")) && cJSON_IsNumber(j)) {
            // pre-start+length blob: end point converts to a length
            float l = (float)j->valuedouble - v->crop_start;
            v->crop_len = (l < 0.02f || l > 1.0f) ? 1.0f : l;
        }
        // matrix sources (-1 off / 0..7). A pre-matrix v2 blob carries the
        // legacy single-dest "cv67" key instead — migrate it to the slot it
        // used to drive (source was fixed at CV6/CV7 per voice back then).
        bool have_matrix = false;
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "m_sp")) && cJSON_IsNumber(j)) {
            v->src_speed = (j->valueint >= 0 && j->valueint <= 7) ? j->valueint : -1;
            have_matrix = true;
        }
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "m_st")) && cJSON_IsNumber(j))
            v->src_start = (j->valueint >= 0 && j->valueint <= 7) ? j->valueint : -1;
        if (((j = cJSON_GetObjectItemCaseSensitive(vo, "m_ln")) && cJSON_IsNumber(j)) ||
            ((j = cJSON_GetObjectItemCaseSensitive(vo, "m_en")) && cJSON_IsNumber(j)))
            v->src_len = (j->valueint >= 0 && j->valueint <= 7) ? j->valueint : -1;
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "cm")) && cJSON_IsNumber(j))
            v->crop_mode = (j->valueint >= 0 && j->valueint <= 3) ? j->valueint
                                                                  : S3_CROP_FREE;
        if (!have_matrix &&
            (j = cJSON_GetObjectItemCaseSensitive(vo, "cv67")) && cJSON_IsNumber(j)) {
            v->src_speed = -1;
            v->src_start = -1;
            v->src_len = -1;
            switch (j->valueint) {
                case 1: v->src_speed = 5 + i; break;
                case 2: v->src_start = 5 + i; break;
                case 3: v->src_len   = 5 + i; break;
            }
        }
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "level")) && cJSON_IsNumber(j)) {
            float lv = (float)j->valuedouble;
            v->level = (lv < 0 || lv > 1.0f) ? 1.0f : lv;
        }
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "pan")) && cJSON_IsNumber(j)) {
            float pn = (float)j->valuedouble;
            v->pan = (pn < -1.0f || pn > 1.0f) ? 0 : pn;
        }
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "name")) && cJSON_IsString(j) &&
            j->valuestring[0] && strcmp(j->valuestring, v->name) != 0)
            s3_load_sample(i, j->valuestring);
    }
}

extern const machine_ui_t s3_menu_ui;

const machine_t machine_sampler3 = {
    .name = "Sampler",
    .start = s3_start,
    .stop = s3_stop,
    .process = s3_process,
    .preset_save = s3_preset_save,
    .preset_load = s3_preset_load,
    .ui = &s3_menu_ui,
};
