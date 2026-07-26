// M5 glitch engine — live-input stutter. Passes audio through; on trigger it
// freezes and loops the most-recent window with pitch + reverse.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "machine.h"
#include "cvsmooth.h"
#include "audio.h"
#include "glitch_priv.h"

gl_state_t gl;

// window length in frames: a clock division when synced+locked, else knob/ms
static uint32_t window_frames(void)
{
    if (gl.sync && gl.ci.clk.locked && gl.ci.clk.period > 0)
        return gl.ci.clk.period / (1u << gl.division);   // 1/4 = period .. 1/32 = period/8
    return (uint32_t)gl.win_ms * GL_RATE / 1000;
}

// copy the last win_len frames (ending at the write head) out of the ring into
// the linear window buffer, handling the ring wrap with two memcpys
static void capture_window(void)
{
    uint32_t wl = window_frames();
    if (wl < 64) wl = 64;
    if (wl > GL_MAX_WIN) wl = GL_MAX_WIN;
    gl.win_len = wl;

    uint32_t start = (gl.wpos + GL_RING_FRAMES - wl) % GL_RING_FRAMES;
    uint32_t first = GL_RING_FRAMES - start;
    if (first > wl) first = wl;
    memcpy(gl.win, gl.ring + start * 2, (size_t)first * 2 * sizeof(int16_t));
    if (first < wl)
        memcpy(gl.win + first * 2, gl.ring, (size_t)(wl - first) * 2 * sizeof(int16_t));

    gl.play_pos = gl.reverse ? (double)(wl - 1) : 0.0;
}

static esp_err_t glitch_start(void)
{
    memset(&gl, 0, sizeof(gl));
    gl.ring = heap_caps_malloc((size_t)GL_RING_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    gl.win  = heap_caps_malloc((size_t)GL_MAX_WIN * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!gl.ring || !gl.win) { ESP_LOGE("GLITCH", "PSRAM alloc failed"); return ESP_ERR_NO_MEM; }
    gl.win_ms = 120;
    gl.pitch_cv = 2048;
    gl.level = 255;
    gl.clk_src = 7;     // CV8 default (both trigs are stutter controls)
    gl.division = 1;    // 1/8 note
    clockin_reset(&gl.ci, 1.0f);   // 1 pulse per beat, same as the looper
    audio_status_set_voices("glitch", "");
    return ESP_OK;
}

static void glitch_stop(void)
{
    free(gl.ring); gl.ring = NULL;
    free(gl.win);  gl.win = NULL;
}

static void glitch_process(int32_t out[MACHINE_BLOCK],
                           const int32_t in[MACHINE_BLOCK],
                           const machine_io_t *io)
{
    // conditioned CV (cvsmooth.h): the ADC throws lone outliers (a channel sits at a
    // steady ~1221 and reports ONE sample of 4). CV1 drives a GAIN here, so a spike is
    // a click — and note the >900 floor-gate turns a DOWN-spike into c1 = 0, which maps
    // to UNITY, i.e. a jump to full level. The clock read stays RAW (clockin has its own
    // Schmitt and needs the edge timing).
    static cvmed_t s_med[8];
    int cvm[8];
    for (int k = 0; k < 8; k++) cvm[k] = cvmed_step(&s_med[k], io->cv[k]);

    if (!gl.ring || !gl.win) { memset(out, 0, MACHINE_BLOCK * sizeof(int32_t)); return; }

    // skip a channel that is carrying the CLOCK — see clock_src_is_cv()
    if (!clock_src_is_cv(gl.clk_src, 5))
        gl.win_ms = 20 + (int)((uint32_t)cvm[5] * 480 / 4095);   // knob6 = 20..500 ms
    if (!clock_src_is_cv(gl.clk_src, 6))
        gl.pitch_cv = cvm[6];                                    // knob7 = pitch
    uint16_t c1 = cvm[0] > 900 ? cvm[0] - 900 : 0;        // CV1 jack = level
    gl.level = c1 ? (uint16_t)((uint32_t)c1 * 255 / 3195) : 255;

    // pitch increment (unity plateau around centre)
    if (gl.pitch_cv >= 1843 && gl.pitch_cv <= 2253) gl.inc = 1.0f;
    else if (gl.pitch_cv > 2253) gl.inc = 1.0f + (float)(gl.pitch_cv - 2253) / 1842.0f;
    else                         gl.inc = 0.5f + (float)gl.pitch_cv / 1843.0f * 0.5f;

    // triggers: TR1 held (active low) = momentary stutter; TR2 press = latch
    static uint8_t prev_trig = 0x03;
    uint8_t pressed = prev_trig & (~io->trig_level) & 0x03;
    prev_trig = io->trig_level;
    if (pressed & 2) gl.latch = !gl.latch;
    bool want = !(io->trig_level & 1) || gl.latch;
    if (want && !gl.stutter) { capture_window(); gl.stutter = true; }
    else if (!want && gl.stutter) { gl.stutter = false; }

    int frames = MACHINE_BLOCK / 2;
    // keep the tempo detector running — CV is sampled once per block, so the
    // block-level conditioned feed is timing-identical to the old per-frame tick
    clockin_block(&gl.ci, clock_source_level(gl.clk_src, io), frames);
    for (int f = 0; f < frames; f++) {
        int32_t l, r;
        if (!gl.stutter) {
            // passthrough + write live into the ring
            int16_t il = (int16_t)(in[f * 2] >> 16);
            int16_t ir = (int16_t)(in[f * 2 + 1] >> 16);
            gl.ring[gl.wpos * 2]     = il;
            gl.ring[gl.wpos * 2 + 1] = ir;
            gl.wpos = (gl.wpos + 1) % GL_RING_FRAMES;
            l = il; r = ir;
        } else {
            // loop the captured window with pitch + reverse
            uint32_t i0 = (uint32_t)gl.play_pos;
            uint32_t i1 = i0 + 1; if (i1 >= gl.win_len) i1 = i0;
            float frac = (float)(gl.play_pos - (double)i0);
            l = (int32_t)((float)gl.win[i0 * 2]     + ((float)gl.win[i1 * 2]     - (float)gl.win[i0 * 2])     * frac);
            r = (int32_t)((float)gl.win[i0 * 2 + 1] + ((float)gl.win[i1 * 2 + 1] - (float)gl.win[i0 * 2 + 1]) * frac);
            if (gl.reverse) {
                gl.play_pos -= gl.inc;
                if (gl.play_pos < 0) gl.play_pos += gl.win_len;
            } else {
                gl.play_pos += gl.inc;
                if (gl.play_pos >= gl.win_len) gl.play_pos -= gl.win_len;
            }
        }
        l = (l * gl.level) >> 8;
        r = (r * gl.level) >> 8;
        out[f * 2]     = l << 16;
        out[f * 2 + 1] = r << 16;
    }
}

// ---- preset ---------------------------------------------------------------
static cJSON *glitch_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "win_ms", gl.win_ms);
    cJSON_AddBoolToObject(o, "reverse", gl.reverse);
    cJSON_AddBoolToObject(o, "sync", gl.sync);
    cJSON_AddNumberToObject(o, "division", gl.division);
    cJSON_AddNumberToObject(o, "clk_src", gl.clk_src);
    return o;
}

static void glitch_preset_load(const cJSON *node)
{
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "win_ms")) && cJSON_IsNumber(j)) gl.win_ms = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "reverse"))) gl.reverse = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sync"))) gl.sync = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "division")) && cJSON_IsNumber(j)) gl.division = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "clk_src")) && cJSON_IsNumber(j))
        gl.clk_src = clock_source_clamp_cv_audio(j->valueint);
}

extern const machine_ui_t glitch_menu_ui;

const machine_t machine_glitch = {
    .name = "Glitch",
    .start = glitch_start,
    .stop = glitch_stop,
    .process = glitch_process,
    .preset_save = glitch_preset_save,
    .preset_load = glitch_preset_load,
    .ui = &glitch_menu_ui,
};
