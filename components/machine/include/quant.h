// quant.h — a knob to a step index, with hysteresis. The one pattern for every
// quantized loop control (length ladders, start grids, slice and cell pickers).
//
// The bug this replaces, seen on the Deck, the Tracker and DoubleDecker in
// turn: a start knob quantized into slots the size of the current loop LENGTH
// re-quantizes onto a coarser grid when the length changes and drags the start
// backwards; and a knob resting on a bin edge flips between neighbours on ADC
// ripple. The cure (DoubleDecker 09-xx, Sampler3 s3_cell_adopt): quantize to an
// INDEX, hold the current index until the knob is a fraction of a bin past the
// shared edge, adopt a jump of more than one bin at once, and put the START on
// a grid that depends on the track only, never on the loop length.
//
// Order matters: take the index with hysteresis FIRST, then apply any clamp
// (minimum rung, end of file). Clamping first moves the bin edges the
// hysteresis is measured against and leaves dead knob positions.
#pragma once
#include <stdint.h>

// Continuous cell position (0..nsteps as a float) -> cell index, holding `cur`
// until `pos` is `hyst` cells past the edge shared with the neighbour. cur < 0
// = no current cell: plain truncation. A move of more than one cell is adopted
// at once. No clamping: the caller owns the range.
static inline int quant_cell(float pos, int cur, float hyst)
{
    int li = (int)pos;
    if (pos < 0.0f) li = -1;
    if (cur < 0 || li == cur) return li;
    if (li > cur + 1 || li < cur - 1) return li;      // a real jump: no debate
    float frac = pos - (float)li;
    if (li > cur) return (frac > hyst) ? li : cur;    // crossed upwards
    return ((1.0f - frac) > hyst) ? li : cur;         // crossed downwards
}

// Knob 0..1 -> index in [0, nsteps). hyst as a fraction of one bin: 1/3 is the
// DoubleDecker's feel on wide bins, 0.15 Sampler3's on cells.
static inline int quant_idx(float k01, int nsteps, int cur, float hyst)
{
    if (nsteps < 1) return 0;
    if (k01 < 0.0f) k01 = 0.0f;
    if (k01 > 1.0f) k01 = 1.0f;
    if (cur >= nsteps) cur = nsteps - 1;
    int i = quant_cell(k01 * (float)nsteps, cur, hyst);
    if (i < 0) i = 0;
    if (i >= nsteps) i = nsteps - 1;
    return i;
}

// Hysteresis for a 12-bit knob split into nsteps bins: a third of a bin, but
// never under min_counts (ADC ripple is ~5) nor over max_counts (a wide bin
// should still feel crisp). Returns the fraction quant_idx wants.
static inline float quant_hyst12(int nsteps, int min_counts, int max_counts)
{
    if (nsteps < 1) return 0.0f;
    float bin = 4096.0f / (float)nsteps;
    float h = bin / 3.0f;
    if (h < (float)min_counts) h = (float)min_counts;
    if (h > (float)max_counts) h = (float)max_counts;
    if (h > bin / 2.0f) h = bin / 2.0f;               // never wider than the bin itself
    return h / bin;
}

// A START grid that depends on the span alone: the smallest power-of-two
// multiple of `unit` (frames per beat, steps per beat, ...) that keeps the
// number of bins at or under max_bins. Changing the loop length never moves
// it, so growing a loop extends the end and leaves the start put.
static inline uint32_t quant_grid_step(uint32_t span, uint32_t unit, int max_bins)
{
    if (!unit) return 0;
    uint32_t step = unit;
    for (uint32_t m = 1; m <= 512; m <<= 1) {
        step = m * unit;
        if (span / step <= (uint32_t)max_bins) break;
    }
    return step;
}
