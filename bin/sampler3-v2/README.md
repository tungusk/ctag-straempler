# sampler3-v2 — the clock-time loop recorder, complete

Full-firmware snapshot at `77765a4` (2026-07-12, refreshed with the
performance + polish wave). The "Sampler" machine
grown from the sampler3-v1 rebuild into a performable, phase-exact loop
instrument, plus core fixes that improve every machine on the module.

## What's in it (over sampler3-v1)

**CV matrix** — per-voice Speed / Start / Length destinations, each freely
assigned to any of CV1..8 (Setup rows). ch1/2 floor-trimmed; all matrix
reads median-of-5 conditioned (WiFi ADC spikes). 1V/oct removed by design
(that's the future instrument-sampler machine).

**Crop windows, sampler2 semantics** — START slides the whole window,
LENGTH extends the tail (gives way only at EOF). Modes:
- OFF — window bypassed
- FREE — continuous points
- QUANT — start + length snap to whole beats of the take's tempo stamp
- QUANTx2 — length rides a power-of-2 ladder (1/4..32 beats), start
  anchors to the whole-beat grid independent of length

**Phase-exact gapless looping** — windowed streaming (the ring keeps the
loop; the reader never races past it), a movable 0.5 s loop-start cache
so wraps of ANY window length play from RAM while the stream rewinds
behind them, a ~23 ms equal-power PRE-ROLL crossfade, seam-latched
geometry per pass, and `pos -= W` wraps: the loop period is exactly W to
the sample — verified against a monitored source.

**Recording that survives load** — 6 s PSRAM capture queue (was 5.8 ms!),
batched 4 KB writes, visible drop counter (`d` in /status v1). Takes
recorded while both voices stream scan pristine.

**Clock** — shared conditioned front-end (`clockin_t`): Schmitt + floor +
detector + ghost gate + pulses-per-beat. Record-page **Clock PPQ** setting
(1/2/4/8; TE "sync8" = 2 PPQ — TE counts pulses per BAR). Internal clock
synthesizes the same PPQ. Detector escape covers any wrong-multiple
mis-lock; PPQ changes force a clean relock.

**Core fixes (all machines)** — `pdMS_TO_TICKS(<10 ms)` is ZERO ticks at
100 Hz: every idle reader loop and the armed recording writer were
busy-spins (intermittent dead REST, "arming mutes both tracks"). All
sleep a real tick now. httpd LRU socket purge. REC filename scan is
hinted (no multi-second FAT walk per arm).

**UI** — dated browser, newest first, cap-proof (512 + evict-oldest);
click-toggle Setup rows with `[ value ]` edit brackets; hysteretic-cell
crop shading + pixel-exact marker erase; rate-limited corner tempo;
Live encoder turn disarms an armed track.

## Diagnostics

`/status` v1: `PP s<starve0>/<starve1> h<heals>/<retrigs> d<capture-drops>
j<crop-jitter> p<cursor>` (focused voice). `/files/raw?name=REC_0150`
(bare name — the handler appends .RAW).

## Polish wave (77765a4)

Joint grid snap (hold BOTH gates ~1s: both loops restart on the same
pulse), waveform-in-the-bar UI for deck + sampler3 (black canvas, fat
state border; sampler3's box ends sit AT the crop points — the box IS
the loop), bold 2px sqrt-lifted waveforms, mirrored track-2 panel,
total-length figures, "Machine" affordances, tidy About page, and a
clean version string via version.txt.

## Flash

```
./flash.sh [port]      # defaults to /dev/cu.usbserial-3110
```
