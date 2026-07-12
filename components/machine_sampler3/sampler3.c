// Sampler3 engine — two gate-triggered voices streamed from SD through
// per-voice PSRAM head-cache + ring (see sampler3_priv.h for the model).
// The reader task owns ALL file I/O; process() only consumes buffers and
// flips request flags. Recording start/stop is also deferred to the reader
// (task creation allocates — not allowed in the audio callback).
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "machine.h"
#include "audio.h"
#include "recording.h"
#include "sd_lock.h"
#include "fileio.h"
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

// semitone table, unison at [12] — same hardware-tuned mapping as sampler2
// (1424..2599 window, 49 ADC counts per semitone, clamped to +/-1 octave)
static const float s3_pitch_lut[25] = {
    0.500000f, 0.529732f, 0.561231f, 0.594604f, 0.629961f, 0.667420f, 0.707107f, 0.749154f,
    0.793701f, 0.840896f, 0.890899f, 0.943874f, 1.000000f, 1.059463f, 1.122462f, 1.189207f,
    1.259921f, 1.334840f, 1.414214f, 1.498307f, 1.587401f, 1.681793f, 1.781797f, 1.887749f,
    2.0f
};
static inline float s3_keyboard_pitch(uint16_t cv)
{
    if (cv <= 1426) return s3_pitch_lut[0];
    if (cv >= 2599) return s3_pitch_lut[24];
    return s3_pitch_lut[(cv - 1424) / 49];
}

// ---- reader task -------------------------------------------------------------

#define S3_CHUNK_FRAMES 4096
#define S3_LOW_WATER    (S3_RATE / 4)     // unmute once 0.25 s is buffered

typedef struct {
    FILE *f;
    uint32_t stream_p;      // next playback-order frame the stream will read
} s3_reader_voice_t;

// read `n` playback-order frames starting at frame `p` of voice v into dst
// (interleaved stereo int16). Handles the forward/reverse file mapping.
// Caller provides the DMA staging buffer. Returns frames actually read.
static uint32_t read_playback_frames(s3_voice_t *v, FILE *f, uint32_t p,
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
        fseek(f, file_frame * 4, SEEK_SET);
        size_t got = fread(stage, 4, want, f);
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
    uint32_t start = (uint32_t)(v->start_pct * (float)v->file_frames);
    if (start >= v->file_frames) start = v->file_frames ? v->file_frames - 1 : 0;
    uint32_t len = (uint32_t)(v->len_pct * (float)(v->file_frames - start));
    if (len < 64 && v->file_frames > start) len = v->file_frames - start > 64 ? 64 : v->file_frames - start;
    v->play_start = start;
    v->play_len = len;
    uint32_t hf = len < S3_HEAD_FRAMES ? len : S3_HEAD_FRAMES;
    uint32_t got = 0;
    // head is PSRAM: stream through the DMA stage in chunks
    while (got < hf) {
        uint32_t want = hf - got;
        if (want > S3_CHUNK_FRAMES) want = S3_CHUNK_FRAMES;
        uint32_t r = read_playback_frames(v, rv->f, got, want, stage, stage);
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
        uint32_t got_c = want ? read_playback_frames(v, rv->f, p, want, stage, stage) : 0;
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
                    snprintf(path, sizeof(path), "/sdcard/usr/%s.RAW", v->pending);
                    sd_lock_take();
                    FILE *f = fopen(path, "rb");
                    if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                             v->file_frames = (uint32_t)(sz / 4); }
                    sd_lock_give();
                    if (!f || v->file_frames == 0) {
                        // missing/empty file = voice stays unloaded, never abort
                        ESP_LOGE(TAG, "open %s failed", path);
                        if (f) { sd_lock_take(); fclose(f); sd_lock_give(); }
                    } else {
                        rv[i].f = f;
                        strlcpy(v->name, v->pending, S3_NAME_LEN);
                        rebuild_head(v, &rv[i], stage);
                    }
                }
                v->pos = 0; v->rpos_i = 0;      // engine is parked (head_valid false)
                v->loading = false;
                if (v->autoplay) {              // fresh take: loop immediately —
                    v->autoplay = false;        // or, clock-synced, come in ON a
                    if (v->name[0]) {           // pulse at the in-phase offset
                        if (s3.rec_synced && s3.clk.locked) v->sync_start_req = true;
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

            if (v->retrig_req) {
                v->retrig_req = false;
                v->wpos = v->head_frames;      // stream restarts right after the head
                rv[i].stream_p = v->head_frames;
                if (v->play_len <= v->head_frames) v->loading = false;  // RAM-resident
                worked = true;
            }

            // top up the ring: playback-order frames [head_frames..play_len).
            // Throttle bound is one chunk TIGHTER than the consumer's frame_ok
            // margin (RING - CHUNK): with both at the same bound, a full ring
            // put the play cursor exactly on the reject line — cursor froze,
            // freezing the throttle, muting the voice for good ~3.9s into any
            // long sample (the "short gate"/cutout + runaway starve counts).
            uint32_t lead = v->wpos - v->rpos_i;
            if (rv[i].stream_p < v->play_len && lead < S3_RING_FRAMES - 2 * S3_CHUNK_FRAMES) {
                uint32_t want = v->play_len - rv[i].stream_p;
                if (want > S3_CHUNK_FRAMES) want = S3_CHUNK_FRAMES;
                uint32_t got = read_playback_frames(v, rv[i].f, rv[i].stream_p, want, stage, stage);
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
                if (v->loading && (v->wpos - v->rpos_i >= S3_LOW_WATER || v->wpos >= v->play_len))
                    v->loading = false;
            } else if (v->loading && v->wpos >= v->play_len) {
                v->loading = false;
            }
        }

        if (!worked) vTaskDelay(pdMS_TO_TICKS(5));
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

void s3_set_window(int vid, float start_pct, float len_pct, bool reverse)
{
    if (vid < 0 || vid >= S3_NVOICES) return;
    s3_voice_t *v = &s3.v[vid];
    if (start_pct < 0) start_pct = 0;
    if (start_pct > 0.99f) start_pct = 0.99f;
    if (len_pct < 0.01f) len_pct = 0.01f;
    if (len_pct > 1.0f) len_pct = 1.0f;
    v->start_pct = start_pct;
    v->len_pct = len_pct;
    v->reverse = reverse;
    if (v->name[0]) {
        v->loading = true;
        v->window_req = true;      // reader rebuilds head + stream
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

int s3_list_samples(char (**names)[S3_NAME_LEN])
{
    static char (*list)[S3_NAME_LEN] = NULL;
    static const int MAX = 224;
    if (!list) list = heap_caps_malloc(MAX * S3_NAME_LEN, MALLOC_CAP_SPIRAM);
    int n = 0;
    sd_lock_take();
    DIR *d = opendir("/sdcard/usr");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < MAX) {
            int l = strlen(e->d_name);
            if (l > 4 && strcasecmp(e->d_name + l - 4, ".RAW") == 0 && l - 4 < S3_NAME_LEN) {
                snprintf(list[n], S3_NAME_LEN, "%.*s", l - 4, e->d_name);
                n++;
            }
        }
        closedir(d);
    }
    sd_lock_give();
    // insertion sort — small list, keeps the browser stable/alphabetical
    for (int i = 1; i < n; i++) {
        char tmp[S3_NAME_LEN];
        memcpy(tmp, list[i], S3_NAME_LEN);
        int j = i - 1;
        while (j >= 0 && strcasecmp(list[j], tmp) > 0) {
            memcpy(list[j + 1], list[j], S3_NAME_LEN);
            j--;
        }
        memcpy(list[j + 1], tmp, S3_NAME_LEN);
    }
    *names = list;
    return n;
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
        if (!v->head || !v->ring) {
            ESP_LOGE(TAG, "PSRAM alloc failed (voice %d)", i);
            for (int k = 0; k <= i; k++) { free(s3.v[k].head); free(s3.v[k].ring); }
            return ESP_ERR_NO_MEM;
        }
        v->level = 1.0f;
        v->len_pct = 1.0f;
        v->v1oct = false;
        v->cv67_dest = S3_CV67_SPEED;    // speed-on-knob is how this machine works
        v->playmode = S3_MODE_LOOP;      // preset feel (Arlo): loops by default
        v->cv_floor = 4095;
    }
    s3.clk_src = 7;                      // CV8, same convention as deck/glitch
    s3.clk_base = 4095;                  // floor tracker converges down
    s3.rec_wait_vid = -1;
    clock_reset(&s3.clk);
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
    }
}

// fetch playback-order frame p (caller guarantees validity)
static inline void s3_fetch(const s3_voice_t *v, uint32_t p, float *l, float *r)
{
    const int16_t *s = (p < v->head_frames)
        ? v->head + (size_t)p * 2
        : v->ring + (size_t)(p % S3_RING_FRAMES) * 2;
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
    if (v->loading) return false;
    if (f >= v->wpos) return false;
    if (resident) return true;
    return (v->wpos - f) <= (S3_RING_FRAMES - S3_CHUNK_FRAMES);
}

static void s3_process(int32_t out[MACHINE_BLOCK],
                       const int32_t in[MACHINE_BLOCK],
                       const machine_io_t *io)
{
    static uint8_t prev_trig = 0x03;      // seed idle-high: no phantom edge
    const int frames = MACHINE_BLOCK / 2;

    uint8_t fell = prev_trig & (~io->trig_level) & 0x03;
    prev_trig = io->trig_level;

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
    static bool rapid_press[S3_NVOICES];      // press arrived mid-sequence
    const uint32_t HOLD_ARM = S3_RATE;        // ~1 s
    // sequencer guard: a press following another within this window can NEVER
    // arm — sequenced gates (long, repeating) were tripping the hold gesture,
    // and every other note became an accidental take that overwrote the track
    const uint32_t RAPID_WIN = (uint32_t)(2.5f * S3_RATE);

    for (int i = 0; i < S3_NVOICES; i++) {
        s3_voice_t *v = &s3.v[i];
        bool low = !(io->trig_level & (1 << i));
        if (since_fell[i] < RAPID_WIN) since_fell[i] += (uint32_t)frames;
        if (fell & (1 << i)) {
            rapid_press[i] = since_fell[i] < RAPID_WIN;
            since_fell[i] = 0;
            hold[i] = (uint32_t)frames;
            hold_fired[i] = false;
            rec_started_this_press[i] = false;
            if (recording_get_trig_func(i) == TRIG_FUNC_RECORD) {
                // armed: the gate drives the take instead of the voice. With
                // a locked clock, start lands ON the next pulse and stop on
                // the next whole beat (loop-ready lengths); free-run acts now.
                // trigger/finish are bare atomics — audio-task safe.
                if (!recording_is_active()) {
                    rec_started_this_press[i] = true;
                    if (s3.clk.locked) {
                        s3.rec_wait_vid = i;          // start on the next pulse
                    } else {
                        recording_trigger();
                        s3.rec_synced = false;
                        s3.rec_frames = 0;
                        s3.rec_pulses = 0;
                        s3.rec_first_pulse = 0;
                    }
                } else {
                    if (s3.clk.locked) s3.rec_stop_wait = true;   // stop on the beat
                    else recording_finish();
                }
            } else if (v->name[0] && v->head_valid) {
                if (v->playing) {
                    // gate toggles: press while playing = pause (also makes the
                    // arm-hold graceful — its initial press quiets the track
                    // instead of blasting a retrigger)
                    v->playing = false;
                } else {
                    // trigger: instant from the head cache; reader restarts the stream
                    v->pos = 0;
                    v->playing = true;
                    if (v->play_len > v->head_frames) {
                        v->loading = true;     // parks ring reads until refilled
                        v->retrig_req = true;
                    }
                }
            }
        } else if (low && hold[i] > 0) {
            hold[i] += (uint32_t)frames;
            if (!hold_fired[i] && hold[i] >= HOLD_ARM) {
                hold_fired[i] = true;
                if (recording_is_active() && rec_started_this_press[i]) {
                    s_rec_abort_req = true;    // held through the take: abort it
                } else if (!recording_is_active() && !rapid_press[i]) {
                    s_arm_vid = i;             // idle hold: arm (or disarm) this track
                    s_arm_req = true;
                }
            }
        } else if (!low) {
            hold[i] = 0;
        }
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

        // rate = CV6/7 through-zero speed (when assigned) x optional 1V/oct.
        // Speed curve (Arlo): knob center = unity; CW half sweeps to +150%;
        // CCW half sweeps down THROUGH ZERO into reverse, clamped at -100%.
        // Plateaus at unity and zero make both dependable knob targets.
        float rate = 1.0f;
        if (v->cv67_dest == S3_CV67_SPEED) {
            int pc = io->cv[5 + i];             // CV6 -> voice 1, CV7 -> voice 2
            if (pc >= 2048) rate = 1.0f + 0.5f * (float)(pc - 2048) / 2047.0f;
            else            rate = -1.0f + 2.0f * (float)pc / 2048.0f;
            if (rate > 0.96f && rate < 1.04f) rate = 1.0f;
            if (rate > -0.04f && rate < 0.04f) rate = 0.0f;
        }
        if (v->v1oct) rate *= s3_keyboard_pitch(io->cv[i]);   // ch1/2 jacks

        float lg = (v->pan <= 0) ? 1.0f : 1.0f - v->pan;
        float rg = (v->pan >= 0) ? 1.0f : 1.0f + v->pan;
        lg *= v->level;
        rg *= v->level;

        // whole window in RAM (head + one linear ring pass, never overwritten)
        // -> reverse/bidirectional play is free. Streamed samples can reverse
        // only through the still-buffered window behind the stream head.
        bool resident = (v->play_len <= v->head_frames + S3_RING_FRAMES) &&
                        v->wpos >= v->play_len;

        bool starved = false;
        for (int fno = 0; fno < frames; fno++) {
            // window edges first: forward end wraps (loop) or stops (one-shot);
            // reverse start wraps-to-end only when the whole window is resident
            if (v->playing && v->head_valid) {
                if (rate >= 0 && v->pos >= (double)v->play_len - 1.0) {
                    if (v->playmode == S3_MODE_LOOP) {
                        v->pos = 0;
                        if (!resident && v->play_len > v->head_frames) {
                            v->loading = true;
                            v->retrig_req = true;
                        }
                    } else {
                        v->playing = false;
                    }
                } else if (rate < 0 && v->pos <= 0) {
                    if (v->playmode == S3_MODE_LOOP && resident)
                        v->pos = (double)v->play_len - 1.001;
                    // else: park at 0 until the knob comes back positive
                }
            }
            uint32_t p = (uint32_t)(v->pos < 0 ? 0 : v->pos);
            bool have = s3_frame_ok(v, p, resident) && s3_frame_ok(v, p + 1, resident) &&
                        (p + 1) < v->play_len;
            bool can_play = v->playing && !muted && v->head_valid &&
                            rate != 0.0f && have;
            if (v->playing && !muted && v->head_valid && !have && !v->loading &&
                (p + 1) < v->play_len)
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
            float l = (l0 + (l1 - l0) * fr) * lg * v->out_gain;
            float r = (r0 + (r1 - r0) * fr) * rg * v->out_gain;
            v->last_l = l;
            v->last_r = r;
            mix_l[fno] += l;
            mix_r[fno] += r;
            v->pos += rate;
            if (v->pos < 0) v->pos = 0;
        }
        if (starved) v->dbg_starve++;
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

    // ---- CV clock (deck-pattern conditioning: floor-track + Schmitt +
    // synthesized square into the shared detector; gate scaled to the pulse
    // rate at 4 pulses per beat, the modular norm) ---------------------------
    s3.clk.period_min = (uint32_t)(44100.0f * 60.0f / (320.0f * 4.0f));
    s3.clk.period_max = (uint32_t)(44100.0f * 60.0f / (15.0f * 4.0f));
    int ccv = io->cv[s3.clk_src & 7];
    if (ccv < s3.clk_base) s3.clk_base = ccv;
    else if (s3.clk_base < 4095) s3.clk_base++;
    bool edge = false;
    if (!s3.clk_high) {
        if (ccv >= s3.clk_base + 900) { s3.clk_high = true; edge = true; }
    } else if (ccv < s3.clk_base + 350) {
        s3.clk_high = false;
    }
    uint16_t synth = s3.clk_high ? 4095 : 0;
    for (int fno = 0; fno < frames; fno++) clock_tick(&s3.clk, synth);

    // ghost guard for the SYNC edges: AC-coupled pulse sources (a sound
    // card can't hold DC) ring on the pulse tail and refire the Schmitt;
    // the detector gates those internally, but the raw `edge` used for
    // synced start/stop must too, or takes land ghost-quantized (measured
    // +83.6 ms on a 12-beat take). Accept an edge only ≥3/4 of a locked
    // period after the last accepted one.
    static uint32_t edge_since = 0;
    if (edge_since < (uint32_t)1 << 30) edge_since += (uint32_t)frames;
    if (edge) {
        uint32_t ep = s3.clk.period;
        if (ep != 0 && edge_since < ep - ep / 4) edge = false;   // ghost
        else edge_since = 0;
    }

    // ---- clock-synced capture bookkeeping (audio-task-owned) ----------------
    s3.post_stop_frames += (uint32_t)frames;   // free-runs; reset at synced stop
    if (recording_is_active()) {
        s3.rec_frames += (uint32_t)frames;
        if (edge) {
            if (s3.rec_pulses == 0 && !s3.rec_synced && s3.rec_first_pulse == 0)
                s3.rec_first_pulse = s3.rec_frames;    // grid marker, unsynced start
            s3.rec_pulses++;
            if (s3.rec_stop_wait && (s3.rec_pulses % 4) == 0) {
                recording_finish();                    // stop ON the whole beat
                s3.rec_stop_wait = false;
                s3.post_stop_frames = 0;               // phase ref for sync re-entry
                if (s3.clk.locked && s3.clk.bpm > 0) {
                    s3.rec_bpm = s3.clk.bpm / 4.0f;    // pulse rate -> beat bpm
                    s3.rec_stamp_req = true;           // reader amends the sidecar
                }
            }
        }
        if (s3.rec_stop_wait && !s3.clk.locked) {
            recording_finish();                        // clock died mid-wait
            s3.rec_stop_wait = false;
        }
    }

    // synced re-entry: a fresh clock-synced take starts ON a pulse, its play
    // position offset by the frames elapsed since the stop edge — the loop
    // comes in exactly in phase, however long the save/load took
    for (int i = 0; i < S3_NVOICES; i++) {
        s3_voice_t *v = &s3.v[i];
        if (!v->sync_start_req) continue;
        if (!s3.clk.locked) {                          // clock gone: just start
            v->pos = 0;
            v->playing = true;
            v->sync_start_req = false;
        } else if (edge && v->play_len) {
            v->pos = (double)(s3.post_stop_frames % v->play_len);
            v->playing = true;
            v->sync_start_req = false;
        }
    }

    if (!recording_is_active() && s3.rec_wait_vid >= 0) {
        if (edge && s3.clk.locked) {
            recording_trigger();                       // start ON the pulse:
            s3.rec_synced = true;                      // downbeat = frame 0
            s3.rec_frames = 0;
            s3.rec_pulses = 0;
            s3.rec_first_pulse = 0;
            s3.rec_wait_vid = -1;
        } else if (!s3.clk.locked) {
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
    cJSON *va = cJSON_CreateArray();
    cJSON_AddItemToObject(o, "voices", va);
    for (int i = 0; i < S3_NVOICES; i++) {
        s3_voice_t *v = &s3.v[i];
        cJSON *vo = cJSON_CreateObject();
        cJSON_AddStringToObject(vo, "name", v->name);
        cJSON_AddNumberToObject(vo, "mode", v->playmode);
        cJSON_AddBoolToObject(vo, "rev", v->reverse);
        cJSON_AddNumberToObject(vo, "start", v->start_pct);
        cJSON_AddNumberToObject(vo, "len", v->len_pct);
        cJSON_AddBoolToObject(vo, "v1oct", v->v1oct);
        cJSON_AddNumberToObject(vo, "cv67", v->cv67_dest);
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
    cJSON *va = cJSON_GetObjectItemCaseSensitive(node, "voices");
    for (int i = 0; i < S3_NVOICES; i++) {
        cJSON *vo = va ? cJSON_GetArrayItem(va, i) : NULL;
        if (!vo) continue;
        s3_voice_t *v = &s3.v[i];
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "mode")) && cJSON_IsNumber(j))
            v->playmode = j->valueint == S3_MODE_LOOP ? S3_MODE_LOOP : S3_MODE_ONESHOT;
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "rev"))) v->reverse = cJSON_IsTrue(j);
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "start")) && cJSON_IsNumber(j)) {
            float s = (float)j->valuedouble;
            v->start_pct = (s < 0 || s > 0.99f) ? 0 : s;
        }
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "len")) && cJSON_IsNumber(j)) {
            float l = (float)j->valuedouble;
            v->len_pct = (l < 0.01f || l > 1.0f) ? 1.0f : l;
        }
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "v1oct"))) v->v1oct = cJSON_IsTrue(j);
        if ((j = cJSON_GetObjectItemCaseSensitive(vo, "cv67")) && cJSON_IsNumber(j))
            v->cv67_dest = (j->valueint == S3_CV67_OFF) ? S3_CV67_OFF : S3_CV67_SPEED;
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
