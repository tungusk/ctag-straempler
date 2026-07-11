# deck-v2

The tempo-precision milestone: the deck now detects track BPM exactly and
holds a measured-tight lock. Snapshot of commit `38c95f2` (tag
`deck-v2-20260710`). Flash with `./flash.sh [PORT]` (default
`/dev/cu.usbserial-3110`, `--flash_size detect`).

## What the numbers say (all instrument-verified)

- **BPM detection is exact**: 120.000-BPM reference click track analyzes to
  120.0000 (conf 1.00); 123.400 to 123.4005. Deterministic across runs.
- **Sync lock measured on tape** (two-channel Scarlett capture, deck clicks
  vs clock pulses, `tools/analyze_drift.py`): residual slip +1.2 ms/min
  (0.0025 BPM), 90% of beats within ±7 ms. The old "slow wander" (a full
  slip cycle every ~73 s) is gone.
- **Analysis is fast**: a full-length (5-min-cap) track in ~2 min, typical
  tracks ~1 min — 12× faster reads via raw FatFS (the buffered VFS path was
  the "crawl"), immune to concurrent module use, and it yields to both
  playback and ring refills (no more play-start stutter).

## New since deck-v1.1

- **Analysis precision ladder** (deck_analysis.c): harmonic disambiguation,
  long-lag re-peak at 8/64/256 beats (~0.003 BPM at 120), sub-bin grid
  interpolation, confidence metric (1 − median/peak of the ACF — beatless
  material scores ~0.1, solid grids ~1.0).
- **Sidecar v2**: `dver`/`conf` fields; v1-analyzed tracks auto-upgrade on
  load when Auto BPM is on ("anchors" name reserved for a future tempo-map).
- **Raw-FatFS analysis reads** + DMA-caps buffers; loading/playing both
  pause the analyzer.
- **Displays**: `ana N%` counts up while analyzing; ext BPM readout smoothed
  (rate-limited EMA, ~3 s settle, snaps on real tempo changes — raw edge
  jitter is block-quantization, the clock itself measured ±10 µs).
- **Core fixes riding along**: clip-window audit (every menutft draw resets
  the shared clip; corner affordance no longer garbles), hub-era REC
  indicator gated to the hub page (it was blanking the corner twice a
  second on every other page), httpd aborted-stream hardening (/files list
  + /drop_sample no longer wedge on dead clients), /files/raw serves .JSN
  sidecars.

## Known-remaining (tracked, not blockers)

- Constant +12 ms click-to-pulse offset (trimmable; nudge exists).
- ~7% of beats stray >15 ms (suspected clock-edge glitch → slew excursions).
- Diagnostic serial heartbeats still enabled (env/acf pace + buckets) —
  harmless, serial-only; strip in a later pass.
- Module's R line out gave no signal on the bench (hardware, not firmware).
