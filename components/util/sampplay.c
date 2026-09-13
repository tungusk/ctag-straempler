// Streaming window player — see sampplay.h.
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sampfile.h"
#include "sd_lock.h"
#include "sampplay.h"

static const char *TAG = "SAMPPLAY";

#define SP_DEF_RING   44100          // ~1 s stereo = 176 KB PSRAM
#define SP_CHUNK      2048           // frames per SD burst (8 KB int16 stereo)
#define SP_MIN_WIN    64             // a window shorter than this is not a sound

struct sampplay_s {
    int16_t  *ring;                  // PSRAM, ring_frames * 2 int16
    uint32_t  ring_frames;
    int16_t  *stage;                 // internal RAM: SDMMC DMA CANNOT target PSRAM
    FILE     *f;
    sampfile_t sf;

    volatile uint32_t wpos, rpos;    // monotonic frame counters
    volatile uint32_t in_pt, out_pt;
    volatile uint32_t src;           // next source frame the reader will read
    volatile bool     playing;
    volatile bool     gate;          // reader says the ring is safe to drain
    volatile bool     flush;         // caller asked for a rewind
    volatile bool     run;           // reader task keep-alive
    volatile bool     alive;
    TaskHandle_t      task;
};

// ---- reader task ------------------------------------------------------------
static void fill_once(sampplay_t *p)
{
    uint32_t used  = p->wpos - p->rpos;
    if (used >= p->ring_frames) return;
    uint32_t space = p->ring_frames - used;
    uint32_t want  = space < SP_CHUNK ? space : SP_CHUNK;
    if (want == 0) return;

    // never read past the loop end — wrap to in_pt instead
    uint32_t left = (p->src < p->out_pt) ? (p->out_pt - p->src) : 0;
    if (left == 0) { p->src = p->in_pt; left = p->out_pt - p->in_pt; }
    if (want > left) want = left;
    if (want == 0) return;

    sd_lock_take();
    fseek(p->f, sf_seek_pos(&p->sf, p->src), SEEK_SET);
    size_t got = sampfile_read(p->f, &p->sf, p->stage, want);
    sd_lock_give();
    if (got == 0) { p->src = p->in_pt; return; }   // short read: restart the loop

    // copy into the ring, in up to two runs around the wrap
    uint32_t head = p->wpos % p->ring_frames;
    uint32_t run1 = p->ring_frames - head;
    if (run1 > got) run1 = got;
    memcpy(p->ring + head * 2, p->stage, run1 * 2 * sizeof(int16_t));
    if (got > run1)
        memcpy(p->ring, p->stage + run1 * 2, (got - run1) * 2 * sizeof(int16_t));

    p->wpos += got;
    p->src  += got;
    if (p->src >= p->out_pt) p->src = p->in_pt;
}

static void reader_task(void *pv)
{
    sampplay_t *p = (sampplay_t *)pv;
    p->alive = true;
    while (p->run) {
        if (!p->f || !p->playing) { p->gate = false; vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (p->flush) {
            p->gate  = false;
            vTaskDelay(1);              // let a render in flight finish
            p->rpos  = 0;
            p->wpos  = 0;
            p->src   = p->in_pt;
            p->flush = false;
        }
        fill_once(p);
        // open the gate once there is a cushion, so the first block does not
        // start on an almost-empty ring
        if (!p->gate && (p->wpos - p->rpos) >= p->ring_frames / 2) p->gate = true;
        // full ring: nothing to do until the audio task has drained some
        if ((p->wpos - p->rpos) >= p->ring_frames - SP_CHUNK) vTaskDelay(pdMS_TO_TICKS(5));
        else vTaskDelay(1);
    }
    p->alive = false;
    vTaskDelete(NULL);
}

// ---- lifecycle --------------------------------------------------------------
sampplay_t *sampplay_create(uint32_t ring_frames)
{
    sampplay_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->ring_frames = ring_frames ? ring_frames : SP_DEF_RING;
    p->ring  = heap_caps_malloc(p->ring_frames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    p->stage = heap_caps_malloc(SP_CHUNK * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    if (!p->ring || !p->stage) {
        ESP_LOGE(TAG, "ring/stage alloc failed");
        sampplay_destroy(p);
        return NULL;
    }
    p->run = true;
    // unpinned and modest priority: it touches SD, and pinning file tasks to
    // core 0 is what made WiFi downloads click (see fs_machine.c)
    if (xTaskCreate(reader_task, "sampplay", 4096, p, 4, &p->task) != pdPASS) {
        ESP_LOGE(TAG, "reader task create failed");
        p->run = false;
        sampplay_destroy(p);
        return NULL;
    }
    return p;
}

void sampplay_destroy(sampplay_t *p)
{
    if (!p) return;
    p->playing = false;
    p->gate = false;
    p->run = false;
    for (int i = 0; i < 100 && p->alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    sampplay_close(p);
    if (p->ring)  heap_caps_free(p->ring);
    if (p->stage) heap_caps_free(p->stage);
    free(p);
}

int sampplay_open(sampplay_t *p, const char *name)
{
    if (!p || !name || !name[0]) return -1;
    sampplay_close(p);

    char path[80];
    sample_resolve(name, path, sizeof(path));
    sd_lock_take();
    FILE *f = fopen(path, "rb");
    sampfile_t sf = {0};
    if (f && sampfile_probe(f, &sf) != 0) { fclose(f); f = NULL; }
    sd_lock_give();
    if (!f || sf.frames == 0) {
        if (f) { sd_lock_take(); fclose(f); sd_lock_give(); }
        return -1;
    }
    p->sf = sf;
    p->in_pt = 0;
    p->out_pt = sf.frames;
    p->src = 0;
    p->rpos = p->wpos = 0;
    p->flush = true;
    p->f = f;                       // last: the reader task tests this
    return 0;
}

void sampplay_close(sampplay_t *p)
{
    if (!p || !p->f) return;
    p->playing = false;
    p->gate = false;
    FILE *f = p->f;
    p->f = NULL;                    // first: stops the reader touching it
    vTaskDelay(pdMS_TO_TICKS(30));  // let an in-flight fill_once finish
    sd_lock_take();
    fclose(f);
    sd_lock_give();
    memset(&p->sf, 0, sizeof(p->sf));
}

bool     sampplay_is_open(const sampplay_t *p) { return p && p->f != NULL; }
uint32_t sampplay_frames(const sampplay_t *p)  { return p ? p->sf.frames : 0; }
bool     sampplay_playing(const sampplay_t *p) { return p && p->playing; }

void sampplay_window(sampplay_t *p, uint32_t in, uint32_t out)
{
    if (!p || !p->f) return;
    uint32_t F = p->sf.frames;
    if (out > F) out = F;
    if (in >= out) in = (out > SP_MIN_WIN) ? out - SP_MIN_WIN : 0;
    if (out - in < SP_MIN_WIN) out = (in + SP_MIN_WIN <= F) ? in + SP_MIN_WIN : F;
    if (in == p->in_pt && out == p->out_pt) return;
    p->in_pt = in;
    p->out_pt = out;
    p->flush = true;
}

void sampplay_play(sampplay_t *p, bool on)
{
    if (!p) return;
    if (on && !p->f) return;
    if (on && !p->playing) p->flush = true;   // always start at the window head
    p->playing = on;
    if (!on) p->gate = false;
}

uint32_t sampplay_pos(const sampplay_t *p)
{
    if (!p || !p->f) return 0;
    uint32_t win = p->out_pt - p->in_pt;
    if (win == 0) return p->in_pt;
    return p->in_pt + (p->rpos % win);
}

// ---- audio task -------------------------------------------------------------
void sampplay_render(sampplay_t *p, int32_t *out, int frames, float gain)
{
    if (!p || !p->playing || !p->gate) {
        memset(out, 0, (size_t)frames * 2 * sizeof(int32_t));
        return;
    }
    uint32_t rf = p->ring_frames;
    uint32_t r  = p->rpos;
    uint32_t w  = p->wpos;
    for (int f = 0; f < frames; f++) {
        if (r < w) {
            uint32_t idx = (r % rf) * 2;
            float l  = (float)p->ring[idx]     * gain;
            float rr = (float)p->ring[idx + 1] * gain;
            if (l  >  32767.0f) l  =  32767.0f;
            if (l  < -32768.0f) l  = -32768.0f;
            if (rr >  32767.0f) rr =  32767.0f;
            if (rr < -32768.0f) rr = -32768.0f;
            out[f * 2]     = ((int32_t)l)  << 16;
            out[f * 2 + 1] = ((int32_t)rr) << 16;
            r++;
        } else {
            out[f * 2] = out[f * 2 + 1] = 0;   // underrun: the reader is behind
        }
    }
    p->rpos = r;
}
