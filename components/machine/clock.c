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
}

bool clock_tick(beatclock_t *c, uint16_t cv)
{
    bool edge = false;
    c->since++;
    c->idle++;

    bool high = c->prev_high ? (cv > CLK_LO) : (cv > CLK_HI);
    if (high && !c->prev_high) {
        if (c->since >= PERIOD_MIN && c->since <= PERIOD_MAX) {
            c->ring[c->ring_n % CLK_RING] = c->since;
            c->ring_n++;
            int n = c->ring_n < CLK_RING ? c->ring_n : CLK_RING;
            uint64_t sum = 0;
            for (int i = 0; i < n; i++) sum += c->ring[i];
            c->period = (uint32_t)(sum / n);
            c->bpm = (float)CLK_RATE * 60.0f / (float)c->period;
            c->locked = (n >= 2);
            edge = true;
        }
        c->since = 0;
        c->idle = 0;
    }
    c->prev_high = high;

    if (c->idle > PERIOD_MAX) {   // clock stopped
        c->locked = false;
        c->ring_n = 0;
        c->period = 0;
        c->bpm = 0.0f;
    }
    return edge;
}
