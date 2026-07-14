#pragma once
#include <stdint.h>

// MEDIAN-OF-5 CV conditioning — the shared answer to the ADC spike (bench-caught
// twice now: sampler3's CV matrix, and 2026-07-13 in DoubleDecker, where CV7 is
// the CROSSFADER and a single stray sample yanked the mix gain toward zero for
// one block and snapped back. That is a broadband click on BOTH decks, on any
// track, which a low-pass masks — and it looks like anything but a knob).
//
// The spike is a lone outlier: the channel reads a steady ~1221 and then throws
// ONE sample of 4. A median rejects it outright, where any amount of averaging or
// slewing just smears it across a few blocks and still clicks. Slew AFTER this if
// you want smoothness; do not slew INSTEAD of it.
//
// Call once per block per channel you care about. Header-only, no allocation.

typedef struct { int v[5]; int n; int i; } cvmed_t;

static inline int cvmed_step(cvmed_t *m, int x)
{
    m->v[m->i] = x;
    m->i = (m->i + 1) % 5;
    if (m->n < 5) m->n++;
    // insertion sort of at most 5 values — cheaper than any clever alternative
    int t[5];
    for (int k = 0; k < m->n; k++) t[k] = m->v[k];
    for (int a = 1; a < m->n; a++) {
        int key = t[a], b = a - 1;
        while (b >= 0 && t[b] > key) { t[b + 1] = t[b]; b--; }
        t[b + 1] = key;
    }
    return t[m->n / 2];
}
