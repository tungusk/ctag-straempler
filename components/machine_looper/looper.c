// M2 looper engine — 4 mono PSRAM tracks, CV-clock-synced bar-quantized
// recording. Runs entirely in the audio task's process() callback; the UI
// task pokes commands via the shared lp state (see looper_priv.h).
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "machine.h"
#include "audio.h"
#include "looper_priv.h"

lp_state_t lp;

// ---- clock detection ------------------------------------------------------
// Rising edge on the selected CV channel (hysteresis around mid-scale), period
// averaged over an 8-interval ring, sanity-gated to 20..300 BPM. "period" is
// samples per quarter-note; a bar = 4 quarters.
#define CLK_HI 2400
#define CLK_LO 1600
#define CLK_RING 8
static bool     s_clk_prev_high;
static uint32_t s_clk_since;          // samples since last rising edge
static uint32_t s_clk_ring[CLK_RING];
static int      s_clk_ring_n;
static uint32_t s_clk_period;         // averaged samples/quarter (0 = none)
static uint32_t s_clk_idle;           // samples since last edge (for lock timeout)

// BPM window -> samples/quarter window
#define PERIOD_MIN (LP_RATE * 60 / 300)   // 300 BPM
#define PERIOD_MAX (LP_RATE * 60 / 20)    // 20 BPM

static void clock_reset(void)
{
    s_clk_prev_high = false;
    s_clk_since = 0;
    s_clk_ring_n = 0;
    s_clk_period = 0;
    s_clk_idle = 0;
    lp.bpm = 0.0f;
    lp.locked = false;
}

// clock line as a 0/4095 pseudo-analog level for clock_tick, from either a CV
// channel (used raw, thresholded below) or a trig input. Trig inputs are
// active-low: a clock pulse pulls the line low, which we map to the "high"
// clock state so the same rising-edge detector works for both.
static uint16_t clock_level(const machine_io_t *io)
{
    if (lp.clk_src == LP_CLK_TR1) return (io->trig_level & 1) ? 0 : 4095;
    if (lp.clk_src == LP_CLK_TR2) return (io->trig_level & 2) ? 0 : 4095;
    return io->cv[lp.clk_src & 7];
}

// advance the clock detector by one frame; returns true on a quarter-note edge
static bool clock_tick(uint16_t cv)
{
    bool edge = false;
    s_clk_since++;
    s_clk_idle++;

    bool high = s_clk_prev_high ? (cv > CLK_LO) : (cv > CLK_HI);
    if (high && !s_clk_prev_high) {
        // rising edge
        if (s_clk_since >= PERIOD_MIN && s_clk_since <= PERIOD_MAX) {
            s_clk_ring[s_clk_ring_n % CLK_RING] = s_clk_since;
            s_clk_ring_n++;
            int n = s_clk_ring_n < CLK_RING ? s_clk_ring_n : CLK_RING;
            uint64_t sum = 0;
            for (int i = 0; i < n; i++) sum += s_clk_ring[i];
            s_clk_period = (uint32_t)(sum / n);
            lp.bpm = (float)LP_RATE * 60.0f / (float)s_clk_period;
            lp.locked = (n >= 2);
            edge = true;
        }
        s_clk_since = 0;
        s_clk_idle = 0;
    }
    s_clk_prev_high = high;

    if (s_clk_idle > PERIOD_MAX) {    // clock stopped
        lp.locked = false;
        s_clk_ring_n = 0;
        s_clk_period = 0;
        lp.bpm = 0.0f;
    }
    return edge;
}

// ---- track helpers --------------------------------------------------------
static uint32_t bar_frames(void)
{
    if (!s_clk_period) return 0;
    return s_clk_period * 4u * (uint32_t)(lp.bars > 0 ? lp.bars : 1);
}

static void track_start_record(lp_track_t *t)
{
    t->pos = 0;
    t->len = 0;
    // synced: auto-stop after N bars; unsynced: cap at buffer length
    uint32_t bf = (lp.sync_on && lp.locked) ? bar_frames() : 0;
    if (bf == 0 || bf > LP_BUF_FRAMES) bf = LP_BUF_FRAMES;
    t->target = bf;
    t->state = LP_REC;
}

// ---- lifecycle ------------------------------------------------------------
static esp_err_t looper_start(void)
{
    memset(&lp, 0, sizeof(lp));
    lp.sync_on = true;
    lp.clk_src = LP_CLK_TR1;   // trig input: clean clock edge, no attenuverter
    lp.bars = 4;
    lp.sel = 0;
    for (int i = 0; i < LP_TRACKS; i++) {
        lp.tr[i].buf = heap_caps_malloc(LP_BUF_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!lp.tr[i].buf) {
            ESP_LOGE("LOOPER", "PSRAM alloc failed for track %d", i);
            return ESP_ERR_NO_MEM;
        }
        lp.tr[i].state = LP_EMPTY;
    }
    clock_reset();
    audio_status_set_voices("looper", "");
    return ESP_OK;
}

static void looper_stop(void)
{
    for (int i = 0; i < LP_TRACKS; i++) {
        free(lp.tr[i].buf);
        lp.tr[i].buf = NULL;
    }
}

// consume a queued UI command for track i
static void apply_cmd(int i)
{
    lp_track_t *t = &lp.tr[i];
    if (lp.cmd_clear[i]) {
        lp.cmd_clear[i] = 0;
        t->state = LP_EMPTY;
        t->len = t->pos = t->target = 0;
    }
    if (lp.cmd_action[i]) {
        lp.cmd_action[i] = 0;
        switch (t->state) {
            case LP_EMPTY:
                // arm: wait for the next bar boundary when synced, else record now
                if (lp.sync_on && lp.locked) t->state = LP_ARMED;
                else track_start_record(t);
                break;
            case LP_ARMED:  t->state = LP_EMPTY; break;    // cancel arm
            case LP_REC:    t->len = t->pos; t->pos = 0; t->state = LP_PLAY; break; // punch out early
            case LP_PLAY:   t->state = LP_STOP; break;
            case LP_STOP:   t->pos = 0; t->state = LP_PLAY; break;
        }
    }
}

// ---- audio ---------------------------------------------------------------
// in/out are interleaved stereo int32 (left-justified); one MACHINE_BLOCK =
// 32 stereo frames. Mix all playing tracks to mono, fan out to both channels.
static void looper_process(int32_t out[MACHINE_BLOCK],
                           const int32_t in[MACHINE_BLOCK],
                           const machine_io_t *io)
{
    for (int i = 0; i < LP_TRACKS; i++) apply_cmd(i);

    // TR inputs are ACTIVE LOW: idle reads high (bit set), a gate pulls low.
    // Detect the falling edge (1 -> 0), prev seeded idle-high so a fresh start
    // sees no phantom edge. A trig assigned as the clock source is masked out
    // so it doesn't also fire the action. Any free trig = context action on
    // the selected lane (arm / cancel / punch-out / play / stop cycle).
    uint8_t clk_mask = (lp.clk_src == LP_CLK_TR1) ? 1 :
                       (lp.clk_src == LP_CLK_TR2) ? 2 : 0;
    static uint8_t prev_trig = 0x03;
    uint8_t pressed = prev_trig & (~io->trig_level) & 0x03 & ~clk_mask;
    prev_trig = io->trig_level;
    if (pressed) lp.cmd_action[lp.sel] = 1;

    int frames = MACHINE_BLOCK / 2;
    uint16_t clk = clock_level(io);

    for (int f = 0; f < frames; f++) {
        bool bar_edge = false;
        if (clock_tick(clk)) {
            // a quarter edge; a bar edge is every (4*bars) quarters — approximate
            // by starting armed tracks on any quarter when unsynced-length isn't
            // critical, but gate to bar using the ring count
            if (lp.locked && (s_clk_ring_n % (4 * (lp.bars > 0 ? lp.bars : 1))) == 0)
                bar_edge = true;
        }

        // mono input = (L+R)/2 from the 32-bit left-justified samples
        int32_t l = in[f * 2] >> 16;
        int32_t r = in[f * 2 + 1] >> 16;
        int16_t mono_in = (int16_t)((l + r) / 2);

        int32_t mix = 0;
        for (int i = 0; i < LP_TRACKS; i++) {
            lp_track_t *t = &lp.tr[i];

            if (t->state == LP_ARMED && bar_edge)
                track_start_record(t);

            if (t->state == LP_REC) {
                t->buf[t->pos] = mono_in;
                t->pos++;
                if (t->pos >= t->target) {
                    t->len = t->pos;
                    t->pos = 0;
                    t->state = LP_PLAY;
                }
            } else if (t->state == LP_PLAY && t->len > 0) {
                mix += t->buf[t->pos];
                t->pos++;
                if (t->pos >= t->len) t->pos = 0;
            }
        }

        if (mix > 32767) mix = 32767;
        if (mix < -32768) mix = -32768;
        out[f * 2]     = mix << 16;
        out[f * 2 + 1] = mix << 16;
    }
}

extern const machine_ui_t looper_menu_ui;

const machine_t machine_looper = {
    .name = "Looper",
    .start = looper_start,
    .stop = looper_stop,
    .process = looper_process,
    .ui = &looper_menu_ui,
    // loops are RAM-only in v1; preset_save/load (save-to-library) comes later
};
