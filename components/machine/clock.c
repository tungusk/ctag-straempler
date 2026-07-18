#include "clock.h"
#include "beatlisten.h"

// ---- shared clock-source selection (the looper's clock_level, generalized) ----
// Trigs are active-low: a pulse pulls the line low, mapped to the "high" state.
uint16_t clock_source_level(int src, const machine_io_t *io)
{
    if (src == CLK_SRC_TR1)   return (io->trig_level & 1) ? 0 : 4095;
    if (src == CLK_SRC_TR2)   return (io->trig_level & 2) ? 0 : 4095;
    if (src == CLK_SRC_AUDIO) return beatlisten_level();
    if (src == CLK_SRC_INT || src == CLK_SRC_OFF) return 0;   // no external level
    return io->cv[src & 7];
}

const char *clock_source_name(int src)
{
    static const char *names[CLK_SRC_COUNT] = {
        "CV1", "CV2", "CV3", "CV4", "CV5", "CV6", "CV7", "CV8",
        "TR1", "TR2", "AUDIO", "INT", "OFF"
    };
    return (src >= 0 && src < CLK_SRC_COUNT) ? names[src] : "?";
}

// cycle order: CV1..CV8 -> AUDIO -> INT -> OFF -> (wrap). TR1/TR2 excluded
// (transport). INT = self-clock at manual BPM; OFF = free / un-clocked.
int clock_source_cycle_cv_audio(int src, int dir)
{
    static const int order[] = { 0, 1, 2, 3, 4, 5, 6, 7,
                                 CLK_SRC_AUDIO, CLK_SRC_INT, CLK_SRC_OFF };
    const int n = (int)(sizeof(order) / sizeof(order[0]));
    int i = 0;
    for (int k = 0; k < n; k++) if (order[k] == src) { i = k; break; }
    i = (i + (dir > 0 ? 1 : -1) + n) % n;
    return order[i];
}

int clock_source_clamp_cv_audio(int src)
{
    if (src == CLK_SRC_AUDIO || src == CLK_SRC_INT || src == CLK_SRC_OFF) return src;
    return (src >= 0 && src <= 7) ? src : 7;   // TR/garbage -> CV8 default
}

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
        // window covers ANY sustained cadence well under the lock — not just
        // ~half. A dense clock (8 PPQ) caught mid-plug can mis-lock at 4x+
        // the true period; true pulses then arrive at QUARTER cadence, under
        // the old half-only window, and the spurious guard ate them forever
        // (120 bpm in, 30 shown). Lower bound period/12 still excludes
        // AC-ringing ghosts (a few ms against a full period).
        uint32_t raw = c->since_raw;
        c->since_raw = 0;
        if (c->locked && c->period &&
            raw > c->period / 12 && raw < (c->period * 3) / 5) {
            if (++c->ghost_run >= 6) {         // sustained too-fast cadence
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
    bool changed = (ppb != ci->ppb);
    ci->ppb = ppb;
    // pulse-rate sanity gate scaled from the beat range (15..320 bpm) —
    // with the default 20..300 gate a fast clock's every legitimate
    // interval is rejected and the BPM is assembled from missed edges
    ci->clk.period_min = (uint32_t)(CLK_RATE * 60.0f / (320.0f * ppb));
    ci->clk.period_max = (uint32_t)(CLK_RATE * 60.0f / (15.0f * ppb));
    // a PPQ change is a deliberate moment: drop any lock built under the
    // old convention and relock clean (2 pulses) instead of letting the
    // guards defend a stale period against the new pulse density
    if (changed) {
        ci->clk.ring_n = 0;
        ci->clk.period = 0;
        ci->clk.locked = false;
        ci->clk.bpm = 0.0f;
        ci->clk.ghost_run = 0;
        ci->clk.split_run = 0;
        ci->edge_since = 1u << 30;
    }
}

void clockin_reset(clockin_t *ci, float ppb)
{
    clock_reset(&ci->clk);
    ci->oct = 1.0f;
    ci->base = 4095;              // floor tracker converges down on first reads
    ci->high = false;
    ci->edge_since = 1u << 30;    // first edge is never ghost-gated
    ci->raw_fires = 0;
    ci->raw_iv = 0;
    ci->raw_since = 0;
    clockin_set_ppb(ci, ppb);
}

bool clockin_block(clockin_t *ci, uint16_t cv, int frames)
{
    // floor tracker: dips follow instantly, drift back up slowly
    if ((int)cv < ci->base) ci->base = cv;
    else if (ci->base < 4095) ci->base++;
    ci->raw_since += (uint32_t)frames;
    bool edge = false;
    if (!ci->high) {
        if ((int)cv >= ci->base + 900) {
            ci->high = true; edge = true;
            ci->raw_fires++;
            ci->raw_iv = ci->raw_since;
            ci->raw_since = 0;
        }
    } else if ((int)cv < ci->base + 350) {
        ci->high = false;
    }
    uint16_t synth = ci->high ? 4095 : 0;
    for (int f = 0; f < frames; f++) clock_tick(&ci->clk, synth);

    // OCTAVE PREFERENCE: fold the implied beat tempo into the musical band.
    // Hysteresis band (76..148) is wider than the target (80..140) so a tempo
    // parked near an edge can't flap the fold every pulse.
    if (ci->clk.locked && ci->clk.bpm > 0 && ci->ppb > 0) {
        if (ci->oct <= 0) ci->oct = 1.0f;
        float beat = ci->clk.bpm / (ci->ppb * ci->oct);
        int guard = 0;
        while (beat > 148.0f && ci->oct < 4.0f && guard++ < 4) {
            ci->oct *= 2.0f;                 // beat halves
            beat *= 0.5f;
        }
        guard = 0;
        while (beat < 76.0f && ci->oct > 0.25f && guard++ < 4) {
            ci->oct *= 0.5f;                 // beat doubles
            beat *= 2.0f;
        }
    } else if (!ci->clk.locked) {
        ci->oct = 1.0f;                      // a fresh lock decides afresh
    }
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
