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
}

bool clock_tick(beatclock_t *c, uint16_t cv)
{
    bool edge = false;
    c->since++;
    c->idle++;

    bool high = c->prev_high ? (cv > CLK_LO) : (cv > CLK_HI);
    if (high && !c->prev_high) {
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
            c->period = s[n / 2];
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
