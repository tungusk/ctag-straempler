// Tracker engine — SD-loaded module playback via libxmp. The RENDER task owns
// the libxmp context and fills a PSRAM ring; process() only consumes the ring
// (no libxmp, no SD, no heap in the audio task). Mirrors the deck's producer/
// ring architecture and request-flag protocol.
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "machine.h"
#include "cvsmooth.h"
#include "audio.h"
#include "sd_lock.h"
#include "xmp.h"
#include "trig_gate.h"
#include "tracker_priv.h"

static const char *TAG = "TRACKER";

trk_state_t trk;
const float trk_ppb[5] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
const char *const trk_ppb_names[5] = {"1 per 4 beats", "1 per 2 beats", "1 per beat", "2 per beat", "4 per beat"};

static volatile bool s_run = false, s_alive = false;
static bool s_logged_play = false;   // one-shot stack-watermark log per load
static uint32_t s_render_stack = 0;  // stack the render task actually got (see load gate)
static char s_cur_file[TRK_NAME_LEN] = "";   // module currently loaded/requested

// module extensions libxmp can load (subset shown to the browser + accepted by
// upload). libxmp autodetects the real format from content; the extension is
// just the on-card naming + browser filter.
static const char *const TRK_EXTS[] = {
    ".MOD", ".XM", ".IT", ".S3M", ".669", ".MTM", ".OKT", ".ULT", ".FAR",
    ".MED", ".DBM", ".AMF", ".PTM", ".STM", ".DMF", ".GDM", ".IMF", ".LIQ",
};
#define TRK_N_EXTS (int)(sizeof(TRK_EXTS)/sizeof(TRK_EXTS[0]))

static bool has_mod_ext(const char *name)
{
    int L = strlen(name);
    for (int i = 0; i < TRK_N_EXTS; i++) {
        int el = strlen(TRK_EXTS[i]);
        if (L > el && strcasecmp(name + L - el, TRK_EXTS[i]) == 0) return true;
    }
    return false;
}

// ---- browser list (module files, full name incl. extension) -----------------
static char s_list[192][TRK_NAME_LEN];

static int cmp_name(const void *a, const void *b){ return strcasecmp((const char*)a, (const char*)b); }

int tracker_list_modules(char (**out)[TRK_NAME_LEN])
{
    sd_lock_take();
    DIR *d = opendir(TRK_DIR_VFS);
    int n = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < 192) {
            if (!has_mod_ext(e->d_name)) continue;
            int L = strlen(e->d_name);
            if (L > TRK_NAME_LEN - 1) L = TRK_NAME_LEN - 1;
            memcpy(s_list[n], e->d_name, L);
            s_list[n][L] = 0;
            n++;
        }
        closedir(d);
    }
    sd_lock_give();
    qsort(s_list, n, sizeof(s_list[0]), cmp_name);
    *out = s_list;
    return n;
}

// ---- UI-side controls (set flags; render task acts) -------------------------
void tracker_request_load(const char *name)
{
    strlcpy(trk.file, name, sizeof(trk.file));
    strlcpy(trk.pending, name, sizeof(trk.pending));
    strlcpy(s_cur_file, name, sizeof(s_cur_file));   // track intended module
    trk.playing = false;
    trk.loading = true;
    trk.state = TRK_LOADING;
    trk.nudge_req = 0;
    trk.load_req = true;
}

void tracker_toggle_play(void)
{
    if (trk.state != TRK_READY) return;
    if (trk.playing) { trk.playing = false; return; }
    trk.playing = true;
}

// ---- render task (sole libxmp caller) ---------------------------------------
static xmp_context s_ctx;
static bool s_have_module;
static int16_t s_scratch[TRK_CHUNK * 2];   // internal RAM staging (libxmp -> ring)

static void apply_sound_mode(void)
{
    // Amiga: no interpolation + wide stereo (classic tracker crunch).
    // Clean: cubic spline + gentler stereo blend.
    if (trk.amiga) {
        xmp_set_player(s_ctx, XMP_PLAYER_INTERP, XMP_INTERP_NEAREST);
        xmp_set_player(s_ctx, XMP_PLAYER_MIX, 70);
    } else {
        xmp_set_player(s_ctx, XMP_PLAYER_INTERP, XMP_INTERP_SPLINE);
        xmp_set_player(s_ctx, XMP_PLAYER_MIX, 30);
    }
    trk.sound_dirty = false;
}

// read the whole module file into a PSRAM buffer, chunked under sd_lock with
// the load-bearing per-chunk yield (full-stride SD reads starve the bus).
static void *load_file_psram(const char *name, long *out_size)
{
    char path[80];
    snprintf(path, sizeof(path), TRK_DIR_VFS "/%s", name);
    sd_lock_take();
    FILE *f = fopen(path, "rb");
    long sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET); }
    sd_lock_give();
    if (!f) { strlcpy(trk.fail_why, "open failed", sizeof(trk.fail_why)); return NULL; }
    // SAFETY NET: libxmp's loaders are stack-hungry, and when internal RAM was
    // tight the render task fell back to a TRIMMED stack — on which a heavy
    // module used to overrun and PANIC. Cap the loadable size by the stack we
    // actually got, so a big module is REFUSED cleanly (not a crash). Free
    // internal RAM (switch to Tracker first / reboot) to lift the cap.
    long cap = TRK_MAX_FILE;
    if (s_render_stack && s_render_stack < 32768)
        cap = (s_render_stack < 24000) ? (256L * 1024) : (768L * 1024);
    if (sz <= 0 || sz > cap) {
        sd_lock_take(); fclose(f); sd_lock_give();
        strlcpy(trk.fail_why, sz <= 0 ? "empty" : (cap < TRK_MAX_FILE ? "too big (low RAM)" : "too big"),
                sizeof(trk.fail_why));
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    // stage is INTERNAL (SDMMC DMA can't target PSRAM) — 4 KB, not 16 KB:
    // internal RAM tightened (bc_srv +8 KB for shine) and with the 32 KB
    // render stack just claimed, a 16 KB contiguous internal alloc failed on
    // a FRESH boot ("no memory" with a clean heap, bench 2026-07-17)
    uint8_t *stage = heap_caps_malloc(4096, MALLOC_CAP_DMA);
    if (!buf || !stage) {
        sd_lock_take(); fclose(f); sd_lock_give();
        if (stage) heap_caps_free(stage);
        if (buf) heap_caps_free(buf);
        snprintf(trk.fail_why, sizeof(trk.fail_why), "no mem (%s %ldK)",
                 buf ? "stage" : "file", sz / 1024);
        ESP_LOGE(TAG, "load '%s': %s alloc failed (%ld bytes)",
                 name, buf ? "stage" : "module", buf ? 4096L : sz);
        return NULL;
    }
    long off = 0;
    while (off < sz) {
        long want = sz - off; if (want > 4096) want = 4096;
        sd_lock_take();
        size_t got = fread(stage, 1, want, f);
        sd_lock_give();
        if (got == 0) break;
        memcpy(buf + off, stage, got);
        off += got;
        vTaskDelay(1);                          // load-bearing SD-courtesy gap
    }
    heap_caps_free(stage);
    sd_lock_take(); fclose(f); sd_lock_give();
    if (off < sz) { heap_caps_free(buf); strlcpy(trk.fail_why, "read error", sizeof(trk.fail_why)); return NULL; }
    *out_size = sz;
    return buf;
}

static void do_load(void)
{
    trk.playing = false;
    if (s_have_module) { xmp_end_player(s_ctx); xmp_release_module(s_ctx); s_have_module = false; }
    trk.state = TRK_LOADING;
    trk.loading = true;
    s_logged_play = false;

    long sz = 0;
    void *buf = load_file_psram(trk.pending, &sz);
    if (!buf) { trk.state = TRK_FAIL; return; }

    int r = xmp_load_module_from_memory(s_ctx, buf, sz);
    heap_caps_free(buf);                        // libxmp keeps its own copy
    if (r != 0) {
        snprintf(trk.fail_why, sizeof(trk.fail_why), "load err %d", r);
        trk.state = TRK_FAIL;
        return;
    }

    struct xmp_module_info mi;
    xmp_get_module_info(s_ctx, &mi);
    if (mi.mod && mi.mod->name[0]) strlcpy(trk.title, mi.mod->name, sizeof(trk.title));
    else strlcpy(trk.title, trk.pending, sizeof(trk.title));
    strlcpy(trk.fmt, mi.mod ? mi.mod->type : "?", sizeof(trk.fmt));
    trk.channels = mi.mod ? mi.mod->chn : 0;
    trk.num_pat  = mi.mod ? mi.mod->len : 0;
    trk.mod_bpm  = mi.mod ? mi.mod->bpm : 0;

    // Capture the sample/instrument name slots — composers frequently spell out
    // the song's message/credits across them. Prefer instrument names (XM/IT),
    // fall back to sample names (MOD). Trailing spaces trimmed for tidy display.
    trk.n_names = 0;
    if (mi.mod){
        int use_ins = mi.mod->ins > 0;
        int cnt = use_ins ? mi.mod->ins : mi.mod->smp;
        if (cnt > TRK_MAX_NAMES) cnt = TRK_MAX_NAMES;
        for (int i = 0; i < cnt; i++){
            const char *nm = use_ins ? mi.mod->xxi[i].name : mi.mod->xxs[i].name;
            if (!nm) nm = "";
            strlcpy(trk.names[i], nm, TRK_NM_LEN);
            for (int e = (int)strlen(trk.names[i]) - 1; e >= 0 && (unsigned char)trk.names[i][e] <= ' '; e--)
                trk.names[i][e] = 0;
            trk.n_names++;
        }
    }

    // Build the order-list step map so the loop window can span patterns:
    // order_step0[o] = absolute step (row) at which order o begins.
    trk.n_orders = 0;
    trk.total_steps = 0;
    if (mi.mod){
        struct xmp_module *m = mi.mod;
        int len = m->len; if (len > TRK_MAX_ORDERS) len = TRK_MAX_ORDERS;
        uint32_t acc = 0;
        for (int o = 0; o < len; o++){
            int pat = m->xxo[o];
            if (pat == XMP_MARK_SKIP || pat == XMP_MARK_END) break;   // stop at end/skip
            int rows = (pat < m->pat && m->xxp && m->xxp[pat]) ? m->xxp[pat]->rows : 64;
            if (rows < 1) rows = 1;
            trk.order_rows[o]  = (uint16_t)rows;
            trk.order_step0[o] = acc;
            acc += rows;
            trk.n_orders++;
        }
        trk.total_steps = acc;
    }
    trk.loop_engage = false;        // a fresh module starts in normal play
    trk.loop_toggle_req = false;

    xmp_start_player(s_ctx, TRK_RATE, 0);
    apply_sound_mode();
    trk.tf_cur = 1.0f;
    // stack headroom after the (deepest) load path — verify the 24 KB is enough
    ESP_LOGI(TAG, "loaded '%s' [%s] %dch; render stack low-water=%u B",
             trk.title, trk.fmt, trk.channels, uxTaskGetStackHighWaterMark(NULL));

    trk.wpos = trk.rpos = 0;
    trk.loading = true;
    trk.state = TRK_READY;
    s_have_module = true;
    trk.playing = true;                         // autoplay on load (deck feel)
}

static void ring_flush(void)                    // drop buffered audio, refill
{
    trk.loading = true;
    vTaskDelay(1);                              // 1 tick; pdMS_TO_TICKS(4)==0 at 100Hz
    trk.wpos = trk.rpos = 0;
}

// copy nf stereo int16 frames from src into the PSRAM ring at wpos (wrapping)
static void ring_write(const int16_t *src, int nf)
{
    uint32_t w = trk.wpos % TRK_RING_FRAMES;
    uint32_t first = TRK_RING_FRAMES - w;
    if (first > (uint32_t)nf) first = nf;
    memcpy(trk.ring + w * 2, src, first * 4);
    if (first < (uint32_t)nf)
        memcpy(trk.ring, src + first * 2, (nf - first) * 4);
    trk.wpos += nf;
}

// map an absolute step (row index across the whole order list) → (order, row)
static void abs_to_or(uint32_t astep, int *order, int *row)
{
    int o = 0;
    while (o < trk.n_orders && astep >= trk.order_step0[o] + trk.order_rows[o]) o++;
    if (o >= trk.n_orders) o = trk.n_orders > 0 ? trk.n_orders - 1 : 0;
    *order = o;
    uint32_t base = trk.order_step0[o];
    *row = (int)(astep > base ? astep - base : 0);
    if (trk.order_rows[o] && *row >= trk.order_rows[o]) *row = trk.order_rows[o] - 1;
}

static void render_task(void *pv)
{
    s_ctx = xmp_create_context();
    s_have_module = false;
    s_alive = true;

    bool     cv6_grabbed = false;   // CV6 has MOVED since engage: it owns the window
    int      cv6_applied = -1;      // knob value the current start was computed AT
    uint32_t win_start = 0;         // the window start, in absolute steps — its OWN
                                    // position, so the LENGTH knob can never drag it
    int      loop_engage_ms = 0;    // module time captured when the loop engaged
    uint32_t loop_anchor_w  = 0;    // wpos at engage (frames rendered = real time)
    uint32_t loop_prev_start = 0xFFFFFFFFu;  // last window start (detect a move)
    uint32_t loop_prev_len   = 0;   // last window length (detect a resize)
    bool     loop_resync = false;   // window moved/resized → jump at next step
    int      last_norm_row = -1;    // row tracking for the quantized scrub seek
    uint32_t loop_anchor_abs = 0;   // absolute step where the loop engaged
    int      cv6_engage = 0;        // CV6 at engage — position is relative to it

    while (s_run) {
        if (trk.load_req) { trk.load_req = false; do_load(); continue; }

        // a scrub (encoder) request exits loop mode; the reposition itself is
        // applied on a row boundary in the play path below so it doesn't click
        if (s_have_module && trk.seek_req && trk.loop_engage) trk.loop_engage = false;
        if (s_have_module && trk.restart_req) {
            trk.restart_req = false;
            trk.loop_engage = false;
            trk.nudge_req = 0;
            xmp_restart_module(s_ctx);
            ring_flush();
        }
        // TR2 loop toggle. Engage: snapshot the current module time + write
        // pointer. Release: jump to the phantom position (where linear playback
        // would be now = engage_time + real time elapsed) so the song resumes as
        // if it had never looped. xmp_seek_time maps through the module's own
        // sequence timing, so multi-pattern + position-jump effects are handled.
        if (s_have_module && trk.loop_toggle_req) {
            trk.loop_toggle_req = false;
            if (!trk.loop_engage) {
                struct xmp_frame_info fi; xmp_get_frame_info(s_ctx, &fi);
                loop_engage_ms = fi.time;
                loop_anchor_w  = trk.wpos;
                // the loop grabs the CURRENT playing position; CV6 only shifts it
                // when the knob is actually turned (delta from its value at engage)
                loop_anchor_abs = (fi.pos >= 0 && fi.pos < trk.n_orders)
                                ? trk.order_step0[fi.pos] + (uint32_t)fi.row : 0;
                cv6_engage = trk.loop_pos_cv;
                cv6_grabbed = false;            // dead until the knob actually moves
                cv6_applied = -1;
                win_start = loop_anchor_abs;
                loop_prev_start = 0xFFFFFFFFu;
                // seed the UI loop band at the engage position so it doesn't flash
                // a stale spot for a frame before the first loop render updates it
                {
                    uint32_t tot = trk.total_steps ? trk.total_steps : 1;
                    int ll = trk.loop_len > 0 ? trk.loop_len : 1;
                    trk.loop_a_pm = (int)((uint64_t)loop_anchor_abs * 1000 / tot);
                    trk.loop_b_pm = (int)((uint64_t)(loop_anchor_abs + ll) * 1000 / tot);
                }
                trk.loop_engage = true;
            } else {
                trk.loop_engage = false;
                if (!trk.loop_freeze) {
                    // keeps-running: jump to where linear playback would be now
                    int elapsed_ms = (int)((int64_t)(trk.wpos - loop_anchor_w) * 1000 / TRK_RATE);
                    xmp_seek_time(s_ctx, loop_engage_ms + elapsed_ms);
                    ring_flush();
                }
                // freeze mode: leave the position at the loop, playback continues
            }
        }
        if (s_have_module && trk.sound_dirty) apply_sound_mode();

        // Keep the render only modestly ahead of playback: a deep buffer meant
        // loop edits took "a couple of plays" to be heard. ~0.5 s free-running;
        // ~0.25 s while looping OR synced+locked (convergence S3: halves the
        // nudge latency; revert to /2 if S starts climbing in /status).
        // (Module is fully in PSRAM — no SD latency needs a big read-ahead.)
        uint32_t fill_ahead = (trk.loop_engage ||
                               (trk.sync && trk.ci.clk.locked))
                                  ? (uint32_t)(TRK_RATE / 4)
                                  : (uint32_t)(TRK_RATE / 2);
        bool room = (trk.wpos - trk.rpos) < fill_ahead;
        if (trk.playing && trk.state == TRK_READY && s_have_module && room) {
            struct xmp_frame_info fi;

            if (trk.loop_engage) {
                // Loop mode: render ONE tick at a time so we can wrap exactly on
                // a row boundary. The window spans the whole order list (absolute
                // steps) so it can cross patterns; read live from CV7 (length) +
                // CV6 (position across the song). Tempo-syncs like normal play
                // (the loop rides the external clock too).
                int lb = trk.cur_bpm > 0 ? trk.cur_bpm : trk.mod_bpm;
                if (trk.sync && trk.ci.clk.locked && trk.ci.clk.bpm > 0 && lb > 0) {
                    float ext_beat = trk.ci.clk.bpm / TRK_PPB_EFF();
                    float tgt = (float)lb / ext_beat;   // time factor: live mod bpm / ext
                    if (tgt < 0.5f) tgt = 0.5f;
                    if (tgt > 2.0f) tgt = 2.0f;
                    trk.tf_cur += 0.05f * (tgt - trk.tf_cur);
                } else {
                    trk.tf_cur += 0.05f * (1.0f - trk.tf_cur);
                }
                xmp_set_tempo_factor(s_ctx, trk.tf_cur);
                xmp_play_frame(s_ctx);
                xmp_get_frame_info(s_ctx, &fi);
                ring_write((const int16_t *)fi.buffer, fi.buffer_size / 4);

                int spd = fi.speed > 0 ? fi.speed : 6;
                uint32_t total = trk.total_steps > 0 ? trk.total_steps : 1;
                int len = trk.loop_len;
                if (len < 1) len = 1;
                // GRID SNAP (Arlo: "the tracker loop start needs to figure out
                // where to put the loop start to stay on grid — if clock is
                // active"): while the clock drives us, a window that isn't a
                // whole number of BEATS restarts off-phase and the servo spends
                // the loop fighting it. 24 ticks = one beat at every speed, so
                // rows-per-beat = 24/speed. Snap the LENGTH to whole beats and
                // the start follows for free — it is always a multiple of the
                // length (block math below). Free-running keeps raw row lengths.
                int rpb = 1;
                if (trk.sync && trk.ci.clk.locked) {
                    rpb = 24 / spd;
                    if (rpb < 1) rpb = 1;
                    int snapped = ((len + rpb / 2) / rpb) * rpb;   // nearest beat
                    if (snapped < rpb) snapped = rpb;              // never sub-beat
                    len = snapped;
                }
                if ((uint32_t)len > total) len = (int)total;
                // CV6 = WINDOW POSITION, ABSOLUTE across the WHOLE SONG (Arlo:
                // "cv6 needs to scale to a % and be able to reach the whole
                // track"). It was a DELTA from the knob at engage — one block per
                // ~128 counts — so a full sweep crossed at most 32 blocks and most
                // of a long module was unreachable.
                //
                // The start is its OWN position, snapped to whole BEATS — NOT to
                // whole loop-lengths. Quantising it by the length made the length
                // knob drag it: a longer window re-quantised the same position
                // onto a coarser grid and the start walked backwards (Arlo:
                // "changing the length moves the start point back again" — the
                // identical bug the deck had). Growing a loop must keep the start
                // and extend the END. So the start moves ONLY when CV6 moves.
                uint32_t snap = 1;
                if (trk.sync && trk.ci.clk.locked && rpb > 1) snap = (uint32_t)rpb;
                if (!cv6_grabbed &&
                    (trk.loop_pos_cv - cv6_engage > 120 || cv6_engage - trk.loop_pos_cv > 120)) {
                    cv6_grabbed = true;              // the knob MOVED: it owns the window
                    cv6_applied = -1;                // force a recompute this pass
                }
                if (cv6_grabbed &&
                    (cv6_applied < 0 || trk.loop_pos_cv - cv6_applied > 40
                                     || cv6_applied - trk.loop_pos_cv > 40)) {
                    cv6_applied = trk.loop_pos_cv;
                    uint64_t p = (uint64_t)trk.loop_pos_cv * total / 4096;
                    win_start = (uint32_t)(p / snap) * snap;      // whole beats
                }
                if (!cv6_grabbed) win_start = (loop_anchor_abs / snap) * snap;
                uint32_t lstart = win_start;
                if (lstart + (uint32_t)len > total)                // ran off the end
                    lstart = total > (uint32_t)len ? total - (uint32_t)len : 0;
                lstart = (lstart / snap) * snap;
                uint32_t lend = lstart + (uint32_t)len;
                if (lend > total) lend = total;
                int lo, lr;
                abs_to_or(lstart, &lo, &lr);
                trk.loop_start_ord = lo;
                trk.loop_start_row = lr;
                trk.loop_a_pm = (int)((uint64_t)lstart * 1000 / total);
                trk.loop_b_pm = (int)((uint64_t)lend   * 1000 / total);
                // window moved OR resized (knobs/engage) → re-anchor at the next
                // step boundary — makes both position AND length changes instant
                if (lstart != loop_prev_start || (uint32_t)len != loop_prev_len) {
                    loop_resync = true;
                    loop_prev_start = lstart;
                    loop_prev_len   = (uint32_t)len;
                }
                uint32_t cur_abs = (fi.pos >= 0 && fi.pos < trk.n_orders)
                                 ? trk.order_step0[fi.pos] + (uint32_t)fi.row : 0;
                // relocate ONLY on a row boundary — jumping mid-row while a knob
                // sweeps produces a burst of sub-row note fragments ("catch up")
                if (fi.frame >= spd - 1) {
                    if (loop_resync || cur_abs + 1 >= lend ||
                        cur_abs < lstart || cur_abs >= lend) {
                        // set_row alone within the current order preserves replay
                        // speed; set_position resets it — only re-seat on a real
                        // cross-pattern move
                        if (lo != fi.pos) xmp_set_position(s_ctx, lo);
                        xmp_set_row(s_ctx, lr);
                        loop_resync = false;
                        // the ring frame where the window's first row starts —
                        // the retrig anchors its stutter here
                        trk.loop_wrap_w = trk.wpos;
                    }
                }
            } else {
                // sync: rate-match the module tempo to the external clock via the
                // tempo factor (pitch-preserving). Slew so clock jitter drifts
                // instead of warbling. Nominal factor 1.0 when unsynced/unlocked.
                int live_bpm = trk.cur_bpm > 0 ? trk.cur_bpm : trk.mod_bpm;
                if (trk.sync && trk.ci.clk.locked && trk.ci.clk.bpm > 0 && live_bpm > 0) {
                    float ext_beat = trk.ci.clk.bpm / TRK_PPB_EFF();
                    // xmp_set_tempo_factor is a TIME multiplier (bigger = slower),
                    // bench-verified inverted 2026-07-11 — so the ratio is mod/ext.
                    // LIVE bpm, not load-time: IT songs change tempo mid-song and
                    // a stale ratio makes the servo fight the module.
                    float tgt = (float)live_bpm / ext_beat;
                    // PHASE PULL: rate-matching alone lets the row grid float
                    // against the pulses — also steer rows ONTO the clock.
                    // Beat phase is measured in TICKS (24 ticks = 1 beat holds
                    // for every speed: MOD speed 6 x 4 rows = IT speed 3 x 8
                    // rows) — a row-based phase breaks on IT modules. The
                    // audible position trails the render by the ring lead, so
                    // subtract it. Compare in PULSE phase (like the deck) so
                    // clock mult/div keeps working.
                    if (trk.ph_speed > 0 && trk.ci.clk.period > 0) {
                        float beat_fr = 44100.0f * 60.0f / ext_beat;
                        float ph_r = fmodf((float)trk.ph_row * (float)trk.ph_speed
                                           + (float)trk.ph_frame, 24.0f) / 24.0f;
                        float lead_b = (float)(trk.wpos - trk.rpos) / beat_fr;
                        float p_trk = (ph_r - lead_b) * TRK_PPB_EFF();
                        p_trk -= floorf(p_trk);
                        float p_ext = (float)trk.ci.clk.since / (float)trk.ci.clk.period;
                        if (p_ext > 1.0f) p_ext = 1.0f;
                        float err = p_ext - p_trk;          // >0: rows behind the clock
                        if (err > 0.5f)  err -= 1.0f;
                        if (err < -0.5f) err += 1.0f;
                        if (err > 0.3f)  err = 0.3f;        // bounded pull (~15% bend max)
                        if (err < -0.3f) err = -0.3f;
                        tgt *= (1.0f - 0.5f * err);         // behind -> smaller factor = faster
                    }
                    if (tgt < 0.5f) tgt = 0.5f;
                    if (tgt > 2.0f) tgt = 2.0f;
                    trk.tf_cur += 0.05f * (tgt - trk.tf_cur);
                } else {
                    trk.tf_cur += 0.05f * (1.0f - trk.tf_cur);
                    trk.nudge_req = 0;    // sync dropped: no stale nudge firings
                }
                xmp_set_tempo_factor(s_ctx, trk.tf_cur);

                int r = xmp_play_buffer(s_ctx, s_scratch, TRK_CHUNK * 4, trk.loop ? 0 : 1);
                if (r < 0) trk.playing = false;       // module ended (loop off)
                else       ring_write(s_scratch, TRK_CHUNK);
                xmp_get_frame_info(s_ctx, &fi);
                // apply a pending scrub at the end of the current BAR (every 16th
                // step is a bar boundary), landing on the step that CONTINUES the
                // groove: same row phase as where we left (wrapped into the target
                // pattern) — which at a bar line is itself bar-aligned.
                if (trk.seek_req && last_norm_row >= 0 && fi.row != last_norm_row
                    && (fi.row % 16) == 0) {
                    trk.seek_req = false;
                    xmp_set_position(s_ctx, trk.seek_pos);
                    int tgt_rows = (trk.seek_pos >= 0 && trk.seek_pos < trk.n_orders)
                                 ? trk.order_rows[trk.seek_pos] : 0;
                    if (tgt_rows > 0) {
                        int land = fi.row % tgt_rows;
                        if (land > 0) xmp_set_row(s_ctx, land);
                    }
                    // NO ring_flush: the render is ~0.5 s ahead, so the ring still
                    // holds the tail of the current bar. Flushing would DROP it
                    // (cutting the bar short); instead we let it play out and the
                    // target lands right after, seamlessly on the bar line.
                    trk.nudge_req = 0;             // a scrub obsoletes pending nudges
                }
                // STEP-NUDGE (convergence S2): shift the row grid by whole
                // external-pulse quanta. The servo measures phase mod ONE pulse,
                // so a whole-pulse jump is invisible to it and the nudge STICKS
                // (a fractional jump would be pulled straight back out).
                // Known flag (plan): speeds >12 at ppb x4 overshoot via the
                // min-1-row clamp; the bounded pull absorbs the residue.
                if (trk.nudge_req != 0 && trk.sync && trk.ci.clk.locked &&
                    last_norm_row >= 0 && fi.row != last_norm_row &&
                    fi.speed > 0 && trk.total_steps > 0 &&
                    fi.pos >= 0 && fi.pos < trk.n_orders) {
                    int det = trk.nudge_req;
                    trk.nudge_req = 0;
                    float rpp = (24.0f / trk_ppb[trk.ppb_idx]) / (float)fi.speed;
                    int rows = (int)lroundf(rpp * (float)det);
                    if (rows == 0) rows = det > 0 ? 1 : -1;   // never a no-op
                    int64_t tgt = (int64_t)trk.order_step0[fi.pos] + fi.row + rows;
                    int64_t ts = (int64_t)trk.total_steps;
                    tgt %= ts; if (tgt < 0) tgt += ts;
                    int no, nr;
                    abs_to_or((uint32_t)tgt, &no, &nr);
                    // set_position resets replay speed (house precedent) —
                    // only re-seat on a real cross-pattern move
                    if (no != fi.pos) xmp_set_position(s_ctx, no);
                    xmp_set_row(s_ctx, nr);
                    // NO ring_flush here either — same rationale as the scrub
                }
                last_norm_row = fi.row;
            }

            trk.cur_pos = fi.pos; trk.cur_pat = fi.pattern; trk.cur_row = fi.row;
            trk.time_ms = fi.time; trk.total_ms = fi.total_time; trk.cur_bpm = fi.bpm;
            trk.ph_row = fi.row; trk.ph_frame = fi.frame; trk.ph_speed = fi.speed;
            if (trk.wpos - trk.rpos >= TRK_LOW_WATER) trk.loading = false;
            if (!s_logged_play) {                 // one-shot: stack after first mix
                s_logged_play = true;
                ESP_LOGI(TAG, "first play fill; render stack low-water=%u B",
                         uxTaskGetStackHighWaterMark(NULL));
            }
            continue;                             // keep filling, no delay
        }
        vTaskDelay(1);   // >=1 tick: pdMS_TO_TICKS(5)==0 at 100Hz = busy-spin
    }

    if (s_have_module) { xmp_end_player(s_ctx); xmp_release_module(s_ctx); }
    xmp_free_context(s_ctx);
    s_alive = false;
    vTaskDelete(NULL);
}

// ---- lifecycle --------------------------------------------------------------
static esp_err_t tracker_start(void)
{
    char keep_file[TRK_NAME_LEN];
    bool keep_loop = trk.loop, keep_sync = trk.sync, keep_amiga = trk.amiga;
    int keep_clk = trk.clk_src, keep_ppb = trk.ppb_idx;
    strlcpy(keep_file, trk.file, sizeof(keep_file));   // survive the memset

    memset(&trk, 0, sizeof(trk));
    trk.ring = heap_caps_malloc((size_t)TRK_RING_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!trk.ring) { ESP_LOGE(TAG, "PSRAM ring alloc failed"); return ESP_ERR_NO_MEM; }
    // ensure the module folder exists (harmless if already there); uploads and
    // the browser both live under it
    sd_lock_take();
    mkdir(TRK_DIR_VFS, 0777);
    sd_lock_give();
    trk.loop = keep_loop; trk.sync = keep_sync; trk.amiga = keep_amiga;
    trk.clk_src = keep_clk; trk.ppb_idx = keep_ppb;
    strlcpy(trk.file, keep_file, sizeof(trk.file));
    if (trk.ppb_idx < 0 || trk.ppb_idx > 4) trk.ppb_idx = 4;
    trk.tf_cur = 1.0f;
    trk.loop_len = 4;               // sane until process() reads CV7
    trk.state = TRK_EMPTY;
    // shared front-end: ppb-scaled sanity gates replace the old fixed
    // widened gate (which had to cover 4 PPQN and 1-per-4-beats at once)
    clockin_reset(&trk.ci, trk_ppb[trk.ppb_idx]);

    s_run = true;
    // 32 KB stack: libxmp's loaders overrun the old 8 KB (FreeRTOS
    // stack-overflow / TCB-clobber crashes). Measured peak ~18.7 KB on a plain
    // 4ch MOD (load is the deep path); 32 KB leaves headroom for heavier IT/XM.
    // BUT task stacks are INTERNAL RAM, which has tightened (bc_srv grew for
    // shine) — and an unchecked create failure left the machine WEDGED in
    // LOADING with nothing consuming load_req (bench-caught 2026-07-17, serial
    // showed "starting Tracker" then silence). Descend fail-soft and say so.
    static const uint32_t trk_stacks[] = { 32768, 26624, 22528 };
    bool task_ok = false;
    for (int i = 0; i < 3 && !task_ok; i++) {
        task_ok = xTaskCreate(render_task, "trk_render", trk_stacks[i], NULL, 5, NULL) == pdPASS;
        if (task_ok) {
            s_render_stack = trk_stacks[i];   // gate module size on this (see load_file_psram)
            if (i > 0)
                ESP_LOGW(TAG, "render task on a TRIMMED %u B stack (internal RAM tight) — capping module size", trk_stacks[i]);
        }
    }
    audio_status_set_voices("tracker", "");
    if (!task_ok) {
        ESP_LOGE(TAG, "render task create FAILED at every stack size — no internal RAM");
        strlcpy(trk.fail_why, "no RAM for render", sizeof(trk.fail_why));
        trk.state = TRK_FAIL;
        return ESP_OK;                 // machine switches in; screen says WHY
    }

    if (trk.file[0]) tracker_request_load(trk.file);   // restore last module
    return ESP_OK;
}

static void tracker_stop(void)
{
    trk.playing = false;
    s_run = false;
    for (int i = 0; i < 200 && s_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    free(trk.ring);
    trk.ring = NULL;
}

static void tracker_process(int32_t out[MACHINE_BLOCK], const int32_t in[MACHINE_BLOCK],
                            const machine_io_t *io)
{
    if (!trk.ring) return;

    // conditioned CV (cvsmooth.h): every knob read in this function goes through the
    // median. The clock input is NOT median-filtered — clockin_block has its own
    // Schmitt + period gates and needs the raw edge timing.
    static cvmed_t s_tmed[8];
    static int s_cvm[8];
    for (int k = 0; k < 8; k++) s_cvm[k] = cvmed_step(&s_tmed[k], io->cv[k]);

    // transport gates — the unified TR grammar (convergence S4, shared
    // trig_gate.h): TR1 tap = play/pause (fires on RELEASE now), TR1
    // hold-release = restart from the top; TR2 press = loop toggle (engage
    // ON PRESS — instant, beat-true), TR2 held past the threshold =
    // MOMENTARY loop (the long release toggles back out).
    static trig_gate_t tg1, tg2;
    const int nfr = MACHINE_BLOCK / 2;
    tg_event_t e1 = trig_gate_step_ex(&tg1, !(io->trig_level & 1), io->trig_rising & 1, nfr);
    tg_event_t e2 = trig_gate_step_ex(&tg2, !(io->trig_level & 2), io->trig_rising & 2, nfr);
    if (e1 == TG_REL_SHORT) trk.playing = !trk.playing;
    else if (e1 == TG_REL_LONG) trk.restart_req = true;
    if (e2 == TG_PRESS || e2 == TG_REL_LONG) trk.loop_toggle_req = true;

    // loop window from CV: CV7 (idx 6) = length selector, CV6 (idx 5) = position.
    // Deadband the raw CVs so ADC noise can't jitter length/position — otherwise
    // the loop resizes/relocates every block and retriggers constantly.
    // THE CV7 LADDER. Below one step it stops being a pattern loop and becomes a
    // RETRIG (audio-domain stutter — see retrig_div in the header); at one step
    // and above it is the pattern-cursor loop, now up to 256 steps because a
    // pattern is typically 64 rows and the old 32-step ceiling could not hold even
    // one. Windows clamp to the song and snap to whole beats when synced, so long
    // rungs cap out gracefully on short modules.
    // ...up to 1024 steps (Arlo), which on a typical 64-row pattern is sixteen
    // patterns — most modules end well before that, and the window simply clamps
    // to the song when it does.
    static const int lad_len[15] = {1, 1, 1, 1,  1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    static const int lad_div[15] = {16, 8, 4, 2, 0, 0, 0, 0,  0,  0,  0,   0,   0,   0,    0};
    #define TRK_LEN_STEPS 15
    static int cv_len_h = -1, cv_pos_h = -1;
    int cv_len = s_cvm[6], cv_pos = s_cvm[5];   // median: the +/-60 deadband below is a
                                                // JITTER filter — a 1200-count outlier
                                                // sails straight through it and resizes
                                                // the loop for a block
    if (cv_len_h < 0) cv_len_h = cv_len;
    if (cv_pos_h < 0) cv_pos_h = cv_pos;
    if (cv_len - cv_len_h > 60 || cv_len_h - cv_len > 60) cv_len_h = cv_len;
    if (cv_pos - cv_pos_h > 60 || cv_pos_h - cv_pos > 60) cv_pos_h = cv_pos;
    int li = cv_len_h * TRK_LEN_STEPS / 4096;
    if (li < 0) li = 0;
    if (li > TRK_LEN_STEPS - 1) li = TRK_LEN_STEPS - 1;
    trk.loop_len = lad_len[li];

    trk.retrig_div = trk.loop_engage ? lad_div[li] : 0;   // retrig lives INSIDE the loop
    trk.loop_pos_cv = cv_pos_h;

    // ---- RETRIG: stutter the HEAD OF THE SELECTED ROW.
    // The pattern loop is holding a ONE-STEP window at CV6's position, so the
    // renderer is producing that row over and over. The retrig wraps the first
    // 1/div of each rendered row — which means CV6 chooses WHICH LINE stutters
    // (Arlo: "can retrig be shifted by start position to choose a different line
    // to retrig on"). The cursor keeps ADVANCING, so the renderer keeps running,
    // the clock keeps ticking and the PLL never notices; only the read pointer
    // wraps. A row = `speed` ticks and 24 ticks make a beat at any speed, so
    // step = beat * speed / 24.
    static uint32_t rt_len = 0, rt_base = 0, rt_pending = 0, rt_seen = 0;
    uint32_t want_len = 0;
    if (trk.retrig_div > 0) {
        int bpm = trk.cur_bpm > 20 ? trk.cur_bpm : 125;
        int spd = trk.ph_speed > 0 ? trk.ph_speed : 6;
        float beat_fr = (float)TRK_RATE * 60.0f / (float)bpm;
        float step_fr = beat_fr * (float)spd / 24.0f;
        want_len = (uint32_t)(step_fr / (float)trk.retrig_div);
        if (want_len < 256) want_len = 256;                    // ~6 ms floor
        if (want_len > TRK_RING_FRAMES / 3) want_len = TRK_RING_FRAMES / 3;
    }
    rt_len = want_len;
    trk.retrig_len = rt_len;
    if (!rt_len) { rt_base = 0; rt_pending = 0; }   // released: straight back to play
    // the render task marks the ring frame where the window's first row begins.
    // Adopt it only once the CURSOR reaches it (the renderer runs ahead).
    if (trk.loop_wrap_w != rt_seen) { rt_seen = trk.loop_wrap_w; rt_pending = rt_seen; }
    bool retrig = (rt_len > 0 && rt_base > 0);

    // ---- DJ FILTER (CV6 sweep / CV7 resonance) — what the knobs do when the loop
    // is NOT engaged. The loop BORROWS both knobs; on release each comes back by
    // PASS-THROUGH pickup: inert until it crosses back through the value the engine
    // is still using, so leaving a loop cannot slam the filter (the deck's lesson).
    #define TRK_FLT_FMAX  1.0f
    #define TRK_Q_CLEAN   2.0f
    #define TRK_Q_SQUELCH 0.10f
    #define TRK_PASSTOL   90
    static int pk6 = -2, pk7 = -2;        // -2 live, -1 armed (waiting to be crossed)
    static bool loop_was = false;
    if (trk.loop_engage && !loop_was) { /* engage: the loop takes the knobs */ }
    if (!trk.loop_engage && loop_was) pk6 = pk7 = -1;   // release: arm the pickups
    loop_was = trk.loop_engage;

    if (!trk.loop_engage) {
        int c6 = s_cvm[5], c7 = s_cvm[6];   // MEDIAN, never the raw pin: a lone ADC
                                            // outlier both slams the cutoff AND can land
                                            // within TRK_PASSTOL of the frozen value,
                                            // falsely releasing the pickup that exists
                                            // to stop exactly that slam
        if (pk6 != -2) { int d = c6 - trk.filt_cv;    if (d < 0) d = -d; if (d <= TRK_PASSTOL) pk6 = -2; }
        if (pk7 != -2) { int d = c7 - trk.flt_res_cv; if (d < 0) d = -d; if (d <= TRK_PASSTOL) pk7 = -2; }
        if (pk6 == -2) trk.filt_cv = c6;
        if (pk7 == -2) trk.flt_res_cv = c7;
    }
    {
        int fcv = trk.filt_cv;
        int mode = 0;
        float fc = 0;
        if (fcv < 2048 - 150) {                  // LP zone: sweeps DOWN to the left
            mode = 1;
            float t = (float)fcv / (2048.0f - 150.0f);
            fc = 80.0f * powf(150.0f, t);                    // 80 Hz .. 12 kHz
        } else if (fcv > 2048 + 150) {           // HP zone: sweeps UP to the right
            mode = 2;
            float t = (float)(fcv - 2048 - 150) / (4095.0f - 2048.0f - 150.0f);
            fc = 30.0f * powf(200.0f, t);                    // 30 Hz .. 6 kHz
        }
        trk.flt_mode = mode;
        float f_t = mode ? svf_coef(fc, (float)TRK_RATE, TRK_FLT_FMAX) : 0.0f;
        trk.flt_f += 0.2f * (f_t - trk.flt_f);   // slewed: no zipper on a fast sweep
        // a Chamberlin with low damping AND a high coefficient self-oscillates, so
        // the damping floor has to rise with the cutoff (the drums lesson)
        float q_t = svf_damp((float)trk.flt_res_cv / 4095.0f, TRK_Q_SQUELCH, TRK_Q_CLEAN);
        float qfloor = TRK_Q_SQUELCH + 0.8f * (trk.flt_f > 0.85f ? (trk.flt_f - 0.85f) : 0.0f);
        if (q_t < qfloor) q_t = qfloor;
        trk.flt_q += 0.2f * (q_t - trk.flt_q);
    }
    int fmode = trk.flt_mode;

    int frames = MACHINE_BLOCK / 2;
    static int16_t last_l = 0, last_r = 0;
    bool starved = false;
    for (int fno = 0; fno < frames; fno++) {
        bool can_play = trk.playing && !trk.loading && trk.rpos + 1 < trk.wpos;
        if (!can_play && trk.playing && !trk.loading && !trk.seek_req && trk.wpos > 0
            && trk.rpos + 1 >= trk.wpos)
            starved = true;
        float gt = can_play ? 1.0f : 0.0f;
        trk.out_gain += (gt - trk.out_gain) * 0.015f;
        if (!can_play) {
            last_l = (int16_t)(last_l * 0.94f);
            last_r = (int16_t)(last_r * 0.94f);
            out[fno * 2]     = (int32_t)last_l << 16;
            out[fno * 2 + 1] = (int32_t)last_r << 16;
            continue;
        }
        // adopt the row anchor as the cursor arrives at it
        if (rt_pending && trk.rpos >= rt_pending) { rt_base = rt_pending; rt_pending = 0; }
        retrig = (rt_len > 0 && rt_base > 0 && trk.rpos >= rt_base);
        // RETRIG: wrap the first rt_len frames of the CURRENT ROW. The cursor still
        // advances (so the renderer keeps filling and nothing starves) — only the
        // READ pointer folds back. Frames we re-read sit at most one step behind
        // the cursor, comfortably inside the 2 s ring.
        uint32_t rf = retrig ? (rt_base + ((trk.rpos - rt_base) % rt_len)) : trk.rpos;
        uint32_t i = rf % TRK_RING_FRAMES;
        float fl = (float)trk.ring[i * 2]     * trk.out_gain;
        float fr = (float)trk.ring[i * 2 + 1] * trk.out_gain;
        if (fmode) {                             // the DJ sweep (util/svf.h)
            float lo, hi;
            svf_step(&trk.flt_l, fl, trk.flt_f, trk.flt_q, &lo, NULL, &hi);
            fl = (fmode == 1) ? lo : hi;
            svf_step(&trk.flt_r, fr, trk.flt_f, trk.flt_q, &lo, NULL, &hi);
            fr = (fmode == 1) ? lo : hi;
            if (fl > 32767) fl = 32767;
            if (fl < -32768) fl = -32768;
            if (fr > 32767) fr = 32767;
            if (fr < -32768) fr = -32768;
        } else {
            svf_park(&trk.flt_l, fl);            // park at the signal: no thump when
            svf_park(&trk.flt_r, fr);            // the filter re-engages
        }
        int16_t l = (int16_t)fl, r = (int16_t)fr;
        last_l = l; last_r = r;
        out[fno * 2]     = (int32_t)l << 16;
        out[fno * 2 + 1] = (int32_t)r << 16;
        trk.rpos++;
    }
    if (starved) trk.dbg_starve++;

    // CV clock conditioning: the shared front-end (clockin_t) — floor-tracked
    // Schmitt + ppb-scaled gates; drops the lock for a clean relock when the
    // ppb setting actually changes
    clockin_set_ppb(&trk.ci, TRK_PPB_RAW());   // gates take the RAW setting
    clockin_block(&trk.ci, clock_source_level(trk.clk_src, io), frames);
}

// ---- preset -----------------------------------------------------------------
static cJSON *tracker_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "file", trk.file);
    cJSON_AddBoolToObject(o, "loop", trk.loop);
    cJSON_AddBoolToObject(o, "sync", trk.sync);
    cJSON_AddBoolToObject(o, "amiga", trk.amiga);
    cJSON_AddBoolToObject(o, "show_text", trk.show_text);
    cJSON_AddBoolToObject(o, "loop_freeze", trk.loop_freeze);
    cJSON_AddNumberToObject(o, "clk_src", trk.clk_src);
    cJSON_AddNumberToObject(o, "ppb", trk.ppb_idx);
    return o;
}

static void tracker_preset_load(const cJSON *node)
{
    // defaults first (also the NULL / other-machine-autosave path)
    trk.loop = true; trk.sync = false; trk.amiga = true;
    trk.show_text = false;          // the sample-name panel is opt-in now (Arlo):
                                    // the play bar is the page, not a caption block
    trk.loop_freeze = false;
    trk.clk_src = 7; trk.ppb_idx = 4; trk.file[0] = 0;
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "loop")))  trk.loop  = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sync")))  trk.sync  = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "amiga"))) trk.amiga = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "show_text"))) trk.show_text = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "loop_freeze"))) trk.loop_freeze = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "clk_src")) && cJSON_IsNumber(j))
        trk.clk_src = clock_source_clamp_cv_audio(j->valueint);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "ppb")) && cJSON_IsNumber(j)) {
        trk.ppb_idx = j->valueint; if (trk.ppb_idx < 0) trk.ppb_idx = 0; if (trk.ppb_idx > 4) trk.ppb_idx = 4;
    }
    // file restore happens in start() (needs the render task up) — stash it
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "file")) && cJSON_IsString(j) && j->valuestring[0]) {
        strlcpy(trk.file, j->valuestring, sizeof(trk.file));
        // live change via teleremote (render task already running): hot-reload
        // if the picked module differs from what's loaded. At machine-start
        // restore the render task isn't up yet (s_run false), so start() does
        // the initial load and this stays quiet — avoiding a double load.
        if (s_run && strcmp(trk.file, s_cur_file) != 0)
            tracker_request_load(trk.file);
    }
}

extern const machine_ui_t tracker_menu_ui;

const machine_t machine_tracker = {
    .name = "Tracker",
    .start = tracker_start,
    .stop = tracker_stop,
    .process = tracker_process,
    .preset_save = tracker_preset_save,
    .preset_load = tracker_preset_load,
    .ui = &tracker_menu_ui,
};
