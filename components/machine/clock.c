#include "clock.h"

// Extracted from the looper's inline detector so glitch can beat-sync too.
#define CLK_RATE   44100
#define CLK_HI     1500
#define CLK_LO     800
#define CLK_RING   8
#define PERIOD_MIN (CLK_RATE * 60 / 300)   // 300 BPM
#define PERIOD_MAX (CLK_RATE * 60 / 20)    // 20 BPM

void clock_reset(beatclock_t *c)
{
    c->prev_high = false;
    c->since = 0;
    c->ring_n = 0;
    c->period = 0;
    c->idle = 0;
    c->bpm = 0.0f;
    c->locked = false;
    c->period_min = PERIOD_MIN;
    c->period_max = PERIOD_MAX;
    c->split_run = 0;
    c->ghost_run = 0;
    c->since_raw = 0;
}

bool clock_tick(beatclock_t *c, uint16_t cv)
{
    bool edge = false;
    c->since++;
    c->since_raw++;
    c->idle++;

    bool high = c->prev_high ? (cv > CLK_LO) : (cv > CLK_HI);
    if (high && !c->prev_high) {
        // FASTER-CLOCK ESCAPE: if RAW edges (accepted or not) arrive at a
        // sustained ~half-period cadence, the clock is genuinely faster than
        // the lock — either the rate doubled, or the initial lock caught a
        // missed-edge double (median-of-2 takes the LARGER interval) and the
        // ghost guard then cemented it (every true pulse looked "too early").
        // Raw cadence must be tracked separately from the accumulated
        // interval: ghosted edges don't reset `since`, so accept/ghost
        // alternate and an interval-based run counter never fires.
        uint32_t raw = c->since_raw;
        c->since_raw = 0;
        if (c->locked && c->period &&
            raw > (c->period * 2) / 5 && raw < (c->period * 3) / 5) {
            if (++c->ghost_run >= 6) {         // ~0.75 s of half-period edges
                c->ring_n = 0;                 // full unlock: relock at the
                c->period = 0;                 // true cadence within 2 pulses
                c->locked = false;
                c->bpm = 0.0f;
                c->ghost_run = 0;
            }
        } else {
            c->ghost_run = 0;
        }
        // spurious-edge guard: an edge well inside the locked period is
        // bounce/crosstalk, not a pulse. Ignore it entirely — and keep
        // counting, so the TRUE next edge still measures a full interval
        // (a raw i106 against a 122 ms clock pulled the median down and
        // audibly wobbled the deck's rate)
        if (c->locked && c->period && c->since < (c->period * 3) / 5) {
            c->prev_high = high;
            return false;
        }
        uint32_t iv = c->since;
        // octave guard: a ~2x interval is almost always ONE MISSED EDGE
        // (marginal pulse capture), and believing it halves the tempo — and
        // any rate slaved to it (the deck audibly cut to half speed). Split
        // it as two pulses instead. Escape hatch: 8 consecutive doubles =
        // the artist really halved the tempo, adopt it.
        if (c->locked && c->period &&
            iv > c->period + c->period / 2 && iv < (c->period * 5) / 2) {
            if (++c->split_run >= 8) {
                c->ring_n = 0;               // real tempo change: restart ring
                c->split_run = 0;
            } else {
                iv /= 2;
            }
        } else {
            c->split_run = 0;
        }
        if (iv >= c->period_min && iv <= c->period_max) {
            c->ring[c->ring_n % CLK_RING] = iv;
            c->ring_n++;
            int n = c->ring_n < CLK_RING ? c->ring_n : CLK_RING;
            // MEDIAN, not mean: one missed/doubled edge no longer poisons
            // the estimate for the next 8 pulses
            uint32_t s[CLK_RING];
            for (int i = 0; i < n; i++) s[i] = c->ring[i];
            for (int i = 1; i < n; i++) {
                uint32_t v = s[i];
                int j = i - 1;
                while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
                s[j + 1] = v;
            }
            // LOWER-middle median for even n: missed edges only ever make
            // intervals LONGER, so ties break toward the faster reading —
            // median-of-2 taking the larger let one stray double interval
            // (boot/relock catching a missed pulse) crown itself, and the
            // ghost guard then cemented the half-tempo lock forever
            c->period = s[(n - 1) / 2];
            c->bpm = (float)CLK_RATE * 60.0f / (float)c->period;
            c->locked = (n >= 2);
            edge = true;
        }
        c->since = 0;
        c->idle = 0;
    }
    c->prev_high = high;

    if (c->idle > c->period_max) {   // clock stopped
        c->locked = false;
        c->ring_n = 0;
        c->period = 0;
        c->bpm = 0.0f;
    }
    return edge;
}

// ---- conditioned clock input --------------------------------------------------

void clockin_set_ppb(clockin_t *ci, float ppb)
{
    if (ppb < 0.25f) ppb = 0.25f;
    if (ppb > 24.0f) ppb = 24.0f;
    ci->ppb = ppb;
    // pulse-rate sanity gate scaled from the beat range (15..320 bpm) —
    // with the default 20..300 gate a fast clock's every legitimate
    // interval is rejected and the BPM is assembled from missed edges
    ci->clk.period_min = (uint32_t)(CLK_RATE * 60.0f / (320.0f * ppb));
    ci->clk.period_max = (uint32_t)(CLK_RATE * 60.0f / (15.0f * ppb));
}

void clockin_reset(clockin_t *ci, float ppb)
{
    clock_reset(&ci->clk);
    ci->base = 4095;              // floor tracker converges down on first reads
    ci->high = false;
    ci->edge_since = 1u << 30;    // first edge is never ghost-gated
    clockin_set_ppb(ci, ppb);
}

bool clockin_block(clockin_t *ci, uint16_t cv, int frames)
{
    // floor tracker: dips follow instantly, drift back up slowly
    if ((int)cv < ci->base) ci->base = cv;
    else if (ci->base < 4095) ci->base++;
    bool edge = false;
    if (!ci->high) {
        if ((int)cv >= ci->base + 900) { ci->high = true; edge = true; }
    } else if ((int)cv < ci->base + 350) {
        ci->high = false;
    }
    uint16_t synth = ci->high ? 4095 : 0;
    for (int f = 0; f < frames; f++) clock_tick(&ci->clk, synth);
    // AC-coupled pulse sources ring on the tail and refire the Schmitt:
    // accept a sync edge only >= 3/4 of a locked period after the last
    if (ci->edge_since < (1u << 30)) ci->edge_since += (uint32_t)frames;
    if (edge) {
        uint32_t ep = ci->clk.period;
        if (ep != 0 && ci->edge_since < ep - ep / 4) edge = false;   // ghost
        else ci->edge_since = 0;
    }
    return edge;
}
