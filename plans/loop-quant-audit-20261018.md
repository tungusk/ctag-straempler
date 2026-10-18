# Loop-knob quantization audit (2026-10-18, read-only)

The recurring fault: a loop START knob quantized into slots the size of the
current loop LENGTH. Change the length and the next nudge of the start knob
re-quantizes onto the coarser grid, so the start jumps or walks backwards
("turning up cv7 expands the loop backwards"). Add ADC ripple at a bin edge
and the loop churns while nobody touches anything.

## The fix that works (two copies of it already exist)

**DoubleDecker** `components/machine_dualdeck/dualdeck.c:1052-1114`
- length ladder: knob -> rung INDEX, the current rung is held while the knob
  is within its bin +/- `DD_QHYST` (40 counts), and that hold is applied
  BEFORE the min-quantum bump; a new index must persist 3 blocks; pickup
  stays armed until the computed index equals the live one.
- start: whole beats on a FIXED grid whose step (1,2,4..512 beats) depends only
  on the track span (<=128 bins), never on the loop length; per-bin hysteresis
  bin/3 clamped 10..40; a move that would run past EOF is refused, the start is
  never shifted back to make room.

**Sampler3** `components/machine_sampler3/sampler3.c:590-601` `s3_cell_adopt(pos, cur)`:
hold the index until 15% of a cell past the edge, adopt a >1-cell move at once.
Used by `S3_CROP_QUANT2` (:815-842), start on a beat grid independent of length.

## Where it still bites

1. **Deck CV6 window** `components/machine_deck/deck.c:886-919` — THE offender.
   `nwin = span / llen`, `ns = grid_offset + idx * llen`: the start grid IS the
   loop length. Only guard is "knob moved > 24 counts" (`s_c6_last`), which stops
   CV7 from re-evaluating CV6 but not the re-quantize on the next CV6 nudge. No
   index hysteresis; ~7 counts per bin on a short loop in a long track, so one
   24-count step skips windows; pickup is cvmtx grab, first grab can jump. The
   comment at :893-899 describes the narrower CV7 case only.
2. **Deck CV7 length** `deck.c:860-884` — churn-safe (deadband inside the
   candidate bin) but `lo/hi` are computed AFTER the min-quantum bump (:867), so
   rungs below `minq` can never go "solid": dead knob positions.
3. **Tracker CV6 start** `components/machine_tracker/tracker.c:462-487` — partly
   fixed (start snaps to whole beats from the RAW position, grid independent of
   length). Left: (a) the EOF clamp `lstart = total - len` pulls the start back
   when the length grows near the end (DoubleDecker refuses instead); (b) `snap`
   follows live `fi.speed` and clock lock, so a speed effect or lock change moves
   the start with no knob touch; (c) no bin cap: ~4 counts/beat on long modules,
   the 60-count deadband = ~15-beat steps; (d) the comment at :439-447 is stale
   ("the start follows for free — always a multiple of the length").
4. Slicer slice select `slicer.c:492-507` — raw 0.01 deadband, fixed grid. Not
   this bug; could use the helper.

Not applicable: Looper (no start/length knob), Tape (continuous window, encoder
snaps), Granular, Glitch (continuous).

No shared helper exists (checked util, menu, machine_core, ui; cvmtx has only
takeover + dirty threshold).

## Proposed shared helper (components/util/quant.h)

```c
// knob -> step index with hysteresis. cur < 0 = no current index (plain quantize).
// hyst is a fraction of one bin (DoubleDecker bin/3 ~ 0.33, Sampler3 0.15).
// A move of more than one bin is adopted at once. Apply BEFORE any clamp
// (min rung, EOF) — that order is what fixes Deck CV7's dead rungs.
int quant_idx(float k01, int nsteps, int cur, float hyst);
// fixed start grid: smallest power-of-two beats with span/step <= max_bins
uint32_t quant_grid_step(uint32_t span, uint32_t beat, int max_bins);
```
Plus the two policies the callers keep: a 3-block debounce, and refuse-not-shift
at EOF. Then: Deck CV6 rewritten on the DoubleDecker pattern, Deck CV7 bounds
before the bump, Tracker EOF clamp -> refuse and `snap` frozen while the knob is
not moving, Sampler3's `s3_cell_adopt` replaced by the helper. Bench first: a
synthetic CV sweep with +/-5-count ripple at every bin edge, asserting the index
never flips without a real move.

## Status 10-18 evening: BUILT (`60a9bba`)

`components/machine/include/quant.h` as proposed. Deck CV6/CV7, Tracker CV6 and
Sampler3 are on it; DoubleDecker keeps its inline copy (it was the reference and
its feel is checked). Benches in the hostsim repo: `deck_loop_test` (needs a
card whose `usr/TRK120.JSN` carries `"bpm":120`) and `tracker_loop_test`. The
old Deck code failed them exactly as described above (a 30-count nudge after
growing the length: +4 beats, then −8 on the way back). Both GREEN now; flashed
to .85 the same evening. Not yet played.
