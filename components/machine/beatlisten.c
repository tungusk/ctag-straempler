// beatlisten — audio-derived clock service. Streaming port of the deck's
// offline analysis (deck_analysis.c): onset envelope in 256-frame hops
// (~172 Hz) -> half-wave-rectified 3-band log flux -> autocorrelation over
// the 60..190 BPM lag range with harmonic disambiguation and peak-salience
// confidence -> beat phase by folding the flux into one period (64 bins,
// parabolic interp). The audio task does only the cheap part (two SVFs, band
// sums, grid advance, level synthesis); the model runs at prio 4, unpinned,
// one ACF pass per 500 ms over a PSRAM ring.
//
// Steadiness state machine: SILENT -> LISTEN -> LOCKED <-> FREEWHEEL.
// While LOCKED the octave CANNOT change (candidates fold to the current
// tempo), the period slews <=0.5 BPM per pass (1 BPM/s) and the anchor moves
// <=2% of a beat per pass; a retrack (>=4% off at conf>0.45 for 10 straight
// passes, ~5 s) freewheels the old grid through re-acquisition and then
// commits the new tempo in ONE clean jump.
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "beatlisten.h"
#include "svf.h"

static const char *TAG = "BLISTEN";

#define BL_RATE        44100
#define BL_BLK         32                     // frames per audio block
#define BL_HOP         256                    // frames per envelope hop
#define BL_HOP_BLOCKS  (BL_HOP / BL_BLK)      // 8
#define BL_ENV_RATE    (44100.0f / BL_HOP)    // ~172.27 Hz
#define BL_RING        2756                   // 16 s of hops (PSRAM)
#define BL_COMB_HOPS   1034                   // ~6 s phase-fold window
#define BL_MIN_HOPS    689                    // ~4 s before the first estimate
#define BL_BINS        64

// ACF lag range, 60..190 BPM at the hop rate. Bounds are compile-time so the
// accumulator arrays are sized from the same numbers the loops use.
#define BL_LAG_MIN     54                     // (int)(BL_ENV_RATE*60/190)
#define BL_LAG_MAX     172                    // (int)(BL_ENV_RATE*60/60)

#define BL_PULSE_FR    441.0f                 // 10 ms grid pulse width
#define BL_HOLD_BLK    14                     // ~10 ms onset pulse hold
#define BL_REFRACT_BLK 55                     // ~40 ms onset refractory (PULSE)
#define BL_REFRACT_KICK 165                   // ~120 ms — one pulse per kick
#define BL_REFRACT_FLUX 82                    // ~60 ms — any transient
#define BL_SILENCE     500.0f                 // block |mono| sum ~ -66 dBFS
#define BL_LEAD_FR     0                      // phase-anchor trim (on-device tune)

// output-band fold: LISTEN candidates land in [80,160); LOCKED folds to the
// CURRENT tempo instead, so the band edge can never flap a running grid
#define BL_BPM_LO      80.0f
#define BL_BPM_HI      160.0f

// ---- shared config/status (32-bit reads/writes are atomic on ESP32) ---------
static volatile int      s_mode = BL_OFF;
static volatile int      s_out_ch = 0;          // 0 off, 1 L, 2 R
static volatile uint32_t s_relock_seq = 0;
static volatile int      s_state = BL_ST_OFF;
static volatile float    s_bpm = 0, s_conf = 0;
static volatile int      s_cost_us = 0;

// ---- audio-task side ---------------------------------------------------------
static int      s_amode = BL_OFF;      // mode the audio path last saw
static svf_t    s_flo, s_fmid;
static float    s_coef_lo, s_coef_mid, s_coef_kick;
static float    s_pblk_lo, s_pblk_mid, s_pblk_hi;      // FLUX: prev BLOCK log energies
static float    s_flux_blk, s_flux_ema;                // FLUX: this block's flux + baseline
static float    s_acc_lo, s_acc_mid, s_acc_hi;         // hop band |x| sums
static float    s_plog_lo, s_plog_mid, s_plog_hi;      // previous hop log energies
static int      s_hop_blk;                             // blocks into the hop
static float   *s_ring;                                // PSRAM flux ring
static volatile uint32_t s_whop = 0;                   // hops written, total
static volatile uint32_t s_hop_frames = 0;             // s_frames at the last hop write
static uint32_t s_frames = 0;                          // frames since init
static volatile float s_lvl_fast = 0;                  // block-energy EMA (silence gate)
static float    s_floor = 1e9f;                        // onset noise floor
static int      s_hold = 0, s_refract = 0;             // onset pulse timers
static bool     s_emit = false;                        // grid pulses on?
static float    s_period_fr = 0, s_phase_fr = 0;       // audio-owned grid
static volatile uint16_t s_level = 0;                  // the synthesized clock line

// analysis -> audio handoff (tiny, spinlock-guarded)
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t seq;
    float    period_fr;
    uint32_t anchor_fr;      // a frame KNOWN to be on a beat (recent)
    uint8_t  set_period, set_anchor, emit;
} s_cmd;
static uint32_t s_cmd_applied = 0;

// ---- analysis side -----------------------------------------------------------
static TaskHandle_t s_task = NULL;
static float  *s_scratch;               // linear copy of the ring (PSRAM)
// ACF accumulator + salience sort, statically sized from the lag bounds so a
// heap failure can't silently pin confidence at 0 and block locking (~1.2 KB)
static float   s_rr[BL_LAG_MAX + 2];
static float   s_srt[BL_LAG_MAX - BL_LAG_MIN + 1];
static double  m_now_fr;                // frame of the newest hop in the last pass

void beatlisten_init(void)
{
    s_coef_lo  = 2.0f * sinf((float)M_PI * 150.0f  / BL_RATE);
    s_coef_mid = 2.0f * sinf((float)M_PI * 2000.0f / BL_RATE);
    s_coef_kick = 2.0f * sinf((float)M_PI * 120.0f / BL_RATE);
    svf_reset(&s_flo);
    svf_reset(&s_fmid);
}

uint16_t beatlisten_level(void) { return s_level; }
int  beatlisten_get_mode(void)  { return s_mode; }
int  beatlisten_get_out(void)   { return s_out_ch; }
void beatlisten_relock(void)    { s_relock_seq++; }

void beatlisten_set_out(int ch)
{
    if (ch < 0 || ch > 2) ch = 0;
    s_out_ch = ch;
}

void beatlisten_get_status(bl_status_t *out)
{
    out->mode = s_mode;
    out->state = s_state;
    out->bpm = s_bpm;
    out->conf = s_conf;
    out->cost_us = s_cost_us;
}

// ---- audio task path ----------------------------------------------------------

static void bl_apply_cmd(void)
{
    if (s_cmd.seq == s_cmd_applied) return;
    portENTER_CRITICAL(&s_mux);
    uint32_t seq = s_cmd.seq;
    float    period = s_cmd.period_fr;
    uint32_t anchor = s_cmd.anchor_fr;
    uint8_t  sp = s_cmd.set_period, sa = s_cmd.set_anchor, em = s_cmd.emit;
    portEXIT_CRITICAL(&s_mux);
    s_cmd_applied = seq;
    if (sp && period > 1.0f) s_period_fr = period;
    if (sa && s_period_fr > 1.0f) {
        // anchor is recent (<16 s back), so the delta fits a float exactly
        uint32_t d = s_frames - anchor;
        s_phase_fr = fmodf((float)d, s_period_fr);
    }
    s_emit = em != 0;
    if (!s_emit) s_level = 0;
}

void beatlisten_push(const int32_t in[64])
{
    int mode = s_mode;
    if (mode == BL_OFF) {
        if (s_amode != BL_OFF) {          // just switched off: quiesce
            s_amode = BL_OFF;
            s_level = 0;
            s_emit = false;
        }
        s_cost_us = 0;
        return;
    }
    int64_t t0 = esp_timer_get_time();

    if (mode != s_amode) {                // mode edge: reset the cheap state
        s_amode = mode;
        svf_reset(&s_flo);
        svf_reset(&s_fmid);
        s_acc_lo = s_acc_mid = s_acc_hi = 0;
        s_plog_lo = s_plog_mid = s_plog_hi = 0;
        s_pblk_lo = s_pblk_mid = s_pblk_hi = 0;
        s_flux_blk = s_flux_ema = 0;
        s_hop_blk = 0;
        s_floor = 1e9f;
        s_hold = s_refract = 0;
        s_level = 0;
        // the grid survives a GROOVE->GROOVE relock via the analysis task;
        // entering a non-GROOVE mode kills it
        if (mode != BL_GROOVE) s_emit = false;
    }

    bl_apply_cmd();

    // band/energy pass over the block
    float e_blk = 0;
    if (mode == BL_GROOVE) {
        float alo = 0, amid = 0, ahi = 0;
        for (int f = 0; f < BL_BLK; f++) {
            float mono = 0.5f * ((float)(in[f * 2] >> 16) + (float)(in[f * 2 + 1] >> 16));
            float lo, m2;
            svf_step(&s_flo,  mono, s_coef_lo,  1.0f, &lo, NULL, NULL);
            svf_step(&s_fmid, mono, s_coef_mid, 1.0f, &m2, NULL, NULL);
            alo  += fabsf(lo);
            amid += fabsf(m2 - lo);
            ahi  += fabsf(mono - m2);
        }
        s_acc_lo += alo; s_acc_mid += amid; s_acc_hi += ahi;
        e_blk = alo + amid + ahi;
    } else if (mode == BL_KICK) {
        // low band only: the kick's energy, nothing else's
        for (int f = 0; f < BL_BLK; f++) {
            float mono = 0.5f * ((float)(in[f * 2] >> 16) + (float)(in[f * 2 + 1] >> 16));
            float lo;
            svf_step(&s_flo, mono, s_coef_kick, 1.0f, &lo, NULL, NULL);
            e_blk += fabsf(lo);
        }
    } else if (mode == BL_FLUX) {
        // 3-band rectified log flux, per BLOCK: a spectral CHANGE detector —
        // sustained material contributes nothing, any transient spikes it
        float alo = 0, amid = 0, ahi = 0;
        for (int f = 0; f < BL_BLK; f++) {
            float mono = 0.5f * ((float)(in[f * 2] >> 16) + (float)(in[f * 2 + 1] >> 16));
            float lo, m2;
            svf_step(&s_flo,  mono, s_coef_lo,  1.0f, &lo, NULL, NULL);
            svf_step(&s_fmid, mono, s_coef_mid, 1.0f, &m2, NULL, NULL);
            alo  += fabsf(lo);
            amid += fabsf(m2 - lo);
            ahi  += fabsf(mono - m2);
        }
        float llo = logf(alo + 1.0f), lmi = logf(amid + 1.0f), lhi = logf(ahi + 1.0f);
        float fx = 0, d;
        d = llo - s_pblk_lo;  if (d > 0) fx += d;
        d = lmi - s_pblk_mid; if (d > 0) fx += d;
        d = lhi - s_pblk_hi;  if (d > 0) fx += d;
        s_pblk_lo = llo; s_pblk_mid = lmi; s_pblk_hi = lhi;
        s_flux_blk = fx;
        e_blk = alo + amid + ahi;
    } else {
        for (int f = 0; f < BL_BLK; f++) {
            float mono = 0.5f * ((float)(in[f * 2] >> 16) + (float)(in[f * 2 + 1] >> 16));
            e_blk += fabsf(mono);
        }
    }
    s_lvl_fast += (e_blk - s_lvl_fast) * 0.2f;

    if (mode == BL_GROOVE) {
        // hop bookkeeping: one flux sample per 8 blocks into the PSRAM ring
        if (++s_hop_blk >= BL_HOP_BLOCKS) {
            s_hop_blk = 0;
            float llo = logf(s_acc_lo + 1.0f);
            float lmi = logf(s_acc_mid + 1.0f);
            float lhi = logf(s_acc_hi + 1.0f);
            float flux = 0;
            float d;
            d = llo - s_plog_lo;  if (d > 0) flux += d;
            d = lmi - s_plog_mid; if (d > 0) flux += d;
            d = lhi - s_plog_hi;  if (d > 0) flux += d;
            s_plog_lo = llo; s_plog_mid = lmi; s_plog_hi = lhi;
            s_acc_lo = s_acc_mid = s_acc_hi = 0;
            if (s_ring) {
                s_ring[s_whop % BL_RING] = flux;
                // hop->frame mapping for the anchor math: this hop ends at
                // the last frame of the current block. The pair must change
                // together (a torn pair shifts the anchor by a whole hop), so
                // both stores sit under the handoff spinlock — once per 8
                // blocks, a few hundred ns.
                portENTER_CRITICAL(&s_mux);
                s_hop_frames = s_frames + (uint32_t)BL_BLK;
                s_whop++;
                portEXIT_CRITICAL(&s_mux);
            }
        }
        // grid: free-run on a fractional period; pulse high for the first 10 ms
        if (s_emit && s_period_fr > 1.0f) {
            s_phase_fr += (float)BL_BLK;
            while (s_phase_fr >= s_period_fr) s_phase_fr -= s_period_fr;
            s_level = (s_phase_fr < BL_PULSE_FR) ? 4095 : 0;
        } else {
            s_level = 0;
        }
    } else {
        // onset modes. PULSE/KICK: adaptive energy floor (dips instant, rises
        // slowly) + fire gate. FLUX: the flux fires against its own baseline.
        if (s_refract > 0) s_refract--;
        if (s_hold > 0) s_hold--;
        bool fire;
        if (mode == BL_FLUX) {
            fire = (s_refract == 0 && s_flux_blk > 2.5f * s_flux_ema + 0.8f);
            s_flux_ema += (s_flux_blk - s_flux_ema) * 0.01f;
        } else {
            if (e_blk < s_floor) s_floor = e_blk;
            else s_floor += (e_blk - s_floor) * 0.0005f;
            fire = (s_refract == 0 && e_blk > s_floor * 4.0f + 2000.0f);
        }
        if (fire) {
            s_hold = BL_HOLD_BLK;
            s_refract = (mode == BL_KICK) ? BL_REFRACT_KICK :
                        (mode == BL_FLUX) ? BL_REFRACT_FLUX : BL_REFRACT_BLK;
        }
        s_level = s_hold > 0 ? 4095 : 0;
    }

    s_frames += (uint32_t)BL_BLK;
    int us = (int)(esp_timer_get_time() - t0);
    s_cost_us += (us - s_cost_us) >> 3;
}

void beatlisten_out_render(int32_t out[64])
{
    int ch = s_out_ch;
    if (ch == 0 || s_mode == BL_OFF) return;
    // replace the chosen channel with the click track: full-scale while the
    // clock line is high — the AC-coupled output turns the edge into a spike
    int off = ch - 1;
    int32_t v = s_level ? (int32_t)0x7FFF0000 : 0;
    for (int f = 0; f < BL_BLK; f++) out[f * 2 + off] = v;
}

// ---- analysis task --------------------------------------------------------------

// model state (analysis-owned)
static double   m_anchor;         // frame of a model beat, kept near "now"
static float    m_bpm;            // current grid tempo
static bool     m_have;           // model exists (locked at least once)
static int      m_retrack;        // consecutive confident-disagreement passes
static float    m_cand[3];        // LISTEN: last candidates for the 3-in-a-row gate
static int      m_ncand;
static uint32_t m_valid_from;     // ignore hops before this (relock fence)
static uint32_t m_seen_relock;

static void bl_post(float period_fr, double anchor, bool set_anchor, bool emit)
{
    if (anchor < 0) anchor = 0;      // negative->uint32 cast is UB; clamp
    portENTER_CRITICAL(&s_mux);
    s_cmd.period_fr = period_fr;
    s_cmd.set_period = period_fr > 1.0f;
    s_cmd.anchor_fr = (uint32_t)anchor;
    s_cmd.set_anchor = set_anchor;
    s_cmd.emit = emit;
    s_cmd.seq++;
    portEXIT_CRITICAL(&s_mux);
}

static void bl_model_reset(bool keep_grid)
{
    m_have = false;
    m_retrack = 0;
    m_ncand = 0;
    m_valid_from = s_whop;
    if (!keep_grid) {
        bl_post(0, 0, false, false);
        s_bpm = 0;
    }
    s_conf = 0;
}

static int cmp_float(const void *a, const void *b)
{
    float d = *(const float *)a - *(const float *)b;
    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

// one ACF pass over the freshest window; returns candidate beat BPM (folded
// into the output band when free, to the current tempo when locked) and the
// measured beat anchor in frames. Returns 0 on no usable estimate.
static float bl_acf_pass(uint32_t avail, float *out_conf, double *out_anchor,
                         float fold_to_bpm)
{
    // paired snapshot of (hop index, frame of that hop) under the same
    // spinlock the writer holds — the pair is guaranteed coherent
    uint32_t w, wf;
    portENTER_CRITICAL(&s_mux);
    w = s_whop;
    wf = s_hop_frames;
    portEXIT_CRITICAL(&s_mux);
    uint32_t n = avail > BL_RING ? BL_RING : avail;
    uint32_t start = w - n;
    // linear copy, oldest first (torn floats from the live writer are noise)
    uint32_t s0 = start % BL_RING;
    uint32_t first = BL_RING - s0;
    if (first >= n) {
        memcpy(s_scratch, s_ring + s0, n * sizeof(float));
    } else {
        memcpy(s_scratch, s_ring + s0, first * sizeof(float));
        memcpy(s_scratch + first, s_ring, (n - first) * sizeof(float));
    }

    const int lag_min = BL_LAG_MIN, lag_max = BL_LAG_MAX;
    float best = -1;
    int best_lag = 0;
    for (int lag = lag_min; lag <= lag_max; lag++) {
        float acc = 0;
        const float *e = s_scratch;
        for (uint32_t i = 0; i + lag < n; i++) acc += e[i] * e[i + lag];
        acc /= (float)(n - lag);
        s_rr[lag] = acc;
        float bpm = BL_ENV_RATE * 60.0f / lag;
        float wgt = (bpm >= 80 && bpm <= 165) ? 1.0f : 0.7f;
        if (acc * wgt > best) { best = acc * wgt; best_lag = lag; }
        if ((lag & 7) == 0) vTaskDelay(1);        // don't hog prio 4
    }
    if (best_lag == 0 || s_rr[best_lag] <= 0) return 0;

    // harmonic disambiguation: the true beat lag also scores at its double
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
            while (c > lag_min && s_rr[c - 1] > s_rr[c]) c--;
            while (c < lag_max && s_rr[c + 1] > s_rr[c]) c++;
            float a2 = 0;
            if (2 * c <= lag_max) a2 = s_rr[2 * c];
            else {
                const float *e = s_scratch;
                uint32_t lag2 = (uint32_t)(2 * c);
                if (lag2 < n) {
                    for (uint32_t i = 0; i + lag2 < n; i++) a2 += e[i] * e[i + lag2];
                    a2 /= (float)(n - lag2);
                }
            }
            float bpmc = BL_ENV_RATE * 60.0f / c;
            float wgt = (bpmc >= 80 && bpmc <= 165) ? 1.0f : 0.7f;
            float sc = wgt * (s_rr[c] + 0.5f * a2);
            if (sc > sc_best) { sc_best = sc; chosen = c; }
        }
        best_lag = chosen;
    }

    // peak salience -> confidence (1 - median/peak)
    float conf = 0;
    {
        int nl = lag_max - lag_min + 1;
        memcpy(s_srt, s_rr + lag_min, nl * sizeof(float));
        qsort(s_srt, nl, sizeof(float), cmp_float);
        if (s_rr[best_lag] > 0) conf = 1.0f - s_srt[nl / 2] / s_rr[best_lag];
        if (conf < 0) conf = 0;
        if (conf > 1) conf = 1;
    }
    *out_conf = conf;

    // parabolic refinement around the peak
    float r_best = s_rr[best_lag];
    float r_prev = best_lag > lag_min ? s_rr[best_lag - 1] : r_best;
    float r_next = best_lag < lag_max ? s_rr[best_lag + 1] : r_best;
    float denom = r_prev - 2 * r_best + r_next;
    float shift = (denom != 0) ? 0.5f * (r_prev - r_next) / denom : 0;
    if (shift > 0.5f) shift = 0.5f;
    if (shift < -0.5f) shift = -0.5f;
    float lag_f = (float)best_lag + shift;
    float bpm = BL_ENV_RATE * 60.0f / lag_f;

    // octave fold. Free: into the output band once, here, so the consumer's
    // clockin fold becomes a no-op. Locked: to the CURRENT tempo — an octave
    // jump is impossible inside LOCKED by construction.
    if (fold_to_bpm > 0) {
        int guard = 0;
        while (bpm > fold_to_bpm * 1.5f && guard++ < 4) bpm *= 0.5f;
        guard = 0;
        while (bpm < fold_to_bpm / 1.5f && guard++ < 4) bpm *= 2.0f;
    } else {
        int guard = 0;
        while (bpm >= BL_BPM_HI && guard++ < 4) bpm *= 0.5f;
        guard = 0;
        while (bpm < BL_BPM_LO && guard++ < 4) bpm *= 2.0f;
    }

    // beat phase: fold the freshest ~6 s into one period, strongest bin wins,
    // circular parabolic interp; incremental accumulator, no fmod-per-hop
    double P_h = (double)BL_ENV_RATE * 60.0 / (double)bpm;    // hops per beat
    uint32_t cn = n > BL_COMB_HOPS ? BL_COMB_HOPS : n;
    const float *ce = s_scratch + (n - cn);
    double base = fmod((double)(start + (n - cn)), P_h);
    float bins[BL_BINS];
    memset(bins, 0, sizeof(bins));
    double r = base;
    for (uint32_t i = 0; i < cn; i++) {
        int b = (int)(r / P_h * BL_BINS);
        if (b >= 0 && b < BL_BINS) bins[b] += ce[i];
        r += 1.0;
        if (r >= P_h) r -= P_h;
    }
    int bb = 0;
    for (int b = 1; b < BL_BINS; b++) if (bins[b] > bins[bb]) bb = b;
    float ap = bins[(bb + BL_BINS - 1) % BL_BINS];
    float an = bins[(bb + 1) % BL_BINS];
    float dnb = ap - 2 * bins[bb] + an;
    float shb = (dnb != 0) ? 0.5f * (ap - an) / dnb : 0;
    if (shb > 0.5f) shb = 0.5f;
    if (shb < -0.5f) shb = -0.5f;
    // beats sit at absolute hops H with fmod(H, P_h) == phi
    double phi = ((double)bb + 0.5 + (double)shb) / BL_BINS * P_h;
    double now_h = (double)w;
    double k = floor((now_h - phi) / P_h);
    double beat_h = phi + k * P_h;                 // most recent beat <= now
    // hop -> frame via the paired snapshot (hop w sits at frame wf), so the
    // mapping holds even though s_frames runs in every mode and hops don't
    m_now_fr = (double)wf;
    *out_anchor = (double)wf - (now_h - beat_h) * (double)BL_HOP - (double)BL_LEAD_FR;
    return bpm;
}

static void bl_task(void *pv)
{
    uint32_t last_wf = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));

        // uint32 frame-counter wrap (~27 h of continuous listening): the
        // anchor math assumes a monotonic frame domain, so take one clean
        // relock per wrap instead of an arbitrary phase jump while LOCKED
        uint32_t wf_now = s_hop_frames;
        if (wf_now < last_wf) {
            ESP_LOGI(TAG, "frame counter wrapped — relocking");
            bl_model_reset(m_have && s_mode == BL_GROOVE);
        }
        last_wf = wf_now;

        if (s_relock_seq != m_seen_relock) {
            m_seen_relock = s_relock_seq;
            // manual relock / mode change: keep the grid running only if we
            // were locked in GROOVE (freewheel through re-acquisition)
            bl_model_reset(m_have && s_mode == BL_GROOVE);
        }

        int mode = s_mode;
        if (mode != BL_GROOVE) {
            s_state = (mode == BL_OFF) ? BL_ST_OFF : BL_ST_ONSET;
            s_bpm = 0;
            continue;
        }

        bool have_signal = s_lvl_fast > BL_SILENCE;
        bool emitting = m_have;
        if (!have_signal) {
            s_state = emitting ? BL_ST_FREEWHEEL : BL_ST_SILENT;
            m_ncand = 0;                  // acquisition needs contiguous signal
            continue;
        }

        uint32_t avail = s_whop - m_valid_from;
        if (avail < BL_MIN_HOPS) {
            s_state = emitting ? BL_ST_FREEWHEEL : BL_ST_LISTEN;
            continue;
        }
        if (avail > BL_RING) avail = BL_RING;

        float conf = 0;
        double anchor = 0;
        float cand = bl_acf_pass(avail, &conf, &anchor,
                                 (m_have && m_retrack >= 0) ? m_bpm : 0);
        s_conf = conf;
        if (cand <= 0) { s_state = emitting ? BL_ST_LOCKED : BL_ST_LISTEN; continue; }

        if (!m_have || m_retrack < 0) {
            // (re)acquiring: 3 consecutive estimates within +/-2 BPM at conf
            s_state = emitting ? BL_ST_FREEWHEEL : BL_ST_LISTEN;
            if (conf > 0.35f) {
                // fold toward the PREVIOUS candidate before the consistency
                // test: material near the 160 fold edge otherwise alternates
                // ~159.9 / ~80.05 between passes and 3-in-a-row never lands
                if (m_ncand > 0) {
                    float ref = m_cand[m_ncand - 1];
                    int g = 0;
                    while (cand > ref * 1.5f && g++ < 4) cand *= 0.5f;
                    g = 0;
                    while (cand < ref / 1.5f && g++ < 4) cand *= 2.0f;
                    if (fabsf(cand - ref) > 2.0f) m_ncand = 0;
                }
                if (m_ncand < 3) m_cand[m_ncand++] = cand;
                if (m_ncand >= 3) {
                    m_bpm = cand;
                    m_anchor = anchor;
                    m_have = true;
                    m_retrack = 0;
                    m_ncand = 0;
                    float period = 60.0f * BL_RATE / m_bpm;
                    bl_post(period, m_anchor, true, true);   // ONE clean jump
                    s_bpm = m_bpm;
                    s_state = BL_ST_LOCKED;
                    ESP_LOGI(TAG, "locked %.2f BPM (conf %.2f)", m_bpm, conf);
                }
            } else {
                m_ncand = 0;
            }
            continue;
        }

        // LOCKED: candidate is already folded to the current octave
        s_state = BL_ST_LOCKED;
        float dev = fabsf(cand - m_bpm) / m_bpm;
        if (dev <= 0.04f) {
            m_retrack = 0;
            // tempo slew: <=0.5 BPM per pass (1 BPM/s)
            float d = cand - m_bpm;
            if (d > 0.5f) d = 0.5f;
            if (d < -0.5f) d = -0.5f;
            float nbpm = m_bpm + d;
            float period = 60.0f * BL_RATE / nbpm;
            double P = (double)period;
            // anchor drift: move the model beat toward the measurement by at
            // most 2% of a beat per pass — phase never jumps while locked
            double err = fmod(anchor - m_anchor, P);
            if (err > P * 0.5) err -= P;
            if (err < -P * 0.5) err += P;
            double lim = P * 0.02;
            if (err > lim) err = lim;
            if (err < -lim) err = -lim;
            m_anchor += err;
            m_bpm = nbpm;
            // keep the anchor near "now" so the uint32/float paths stay exact
            while (m_anchor < m_now_fr - 2.0 * P) m_anchor += P;
            while (m_anchor > m_now_fr) m_anchor -= P;
            bl_post(period, m_anchor, true, true);
            s_bpm = m_bpm;
        } else if (conf > 0.45f) {
            if (++m_retrack >= 10) {
                // sustained confident disagreement: re-acquire while the old
                // grid freewheels; the relock commits as one clean jump.
                // FENCE the ring at now — re-acquiring over the mixed window
                // (old tempo still dominates up to 16 s of it) re-locks to
                // the stale tempo and oscillates instead of jumping once.
                ESP_LOGI(TAG, "retrack: %.2f -> ~%.2f BPM candidate", m_bpm, cand);
                m_retrack = -1;           // flag: acquiring with grid alive
                m_ncand = 0;
                m_valid_from = s_whop;
            }
        }
        // low-confidence disagreement (fills, breaks): inertia — do nothing
    }
}

// ---- config entry points ---------------------------------------------------------

void beatlisten_set_mode(int mode)
{
    if (mode < 0 || mode >= BL_NMODES) mode = BL_OFF;
    if (mode != BL_OFF && !s_ring) {
        s_ring = heap_caps_malloc(BL_RING * sizeof(float), MALLOC_CAP_SPIRAM);
        s_scratch = heap_caps_malloc(BL_RING * sizeof(float), MALLOC_CAP_SPIRAM);
        if (!s_ring || !s_scratch) {
            ESP_LOGE(TAG, "PSRAM alloc failed — staying OFF");
            if (s_ring)    { heap_caps_free(s_ring); s_ring = NULL; }
            if (s_scratch) { heap_caps_free(s_scratch); s_scratch = NULL; }
            mode = BL_OFF;
        } else {
            memset(s_ring, 0, BL_RING * sizeof(float));
        }
    }
    if (mode != BL_OFF && !s_task) {
        // unpinned, modest priority — the deck_analysis precedent
        if (xTaskCreate(bl_task, "beatlisten", 4096, NULL, 4, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "task create failed — staying OFF");
            s_task = NULL;
            mode = BL_OFF;
        }
    }
    if (mode != s_mode) s_relock_seq++;   // mode change implies relock
    s_mode = mode;
}
