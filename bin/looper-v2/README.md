# looper-v2 firmware (2026-07-05)

Snapshot of `v09-machines` at commit `b43bb36` (tag `looper-v2-20260705`).
Built with IDF 4.3 + xtensa-esp32-elf esp-2021r2-patch3-8.4.0. Hardware-tested.
Supersedes looper-v1 with a full round of live-playing refinements.

Machines (System -> Machine, persisted): Sampler / Sampler2 / Looper / Stub.

## Looper (M2) — v2

Four-track clock-synced RAM looper, mixed to stereo.

**Clock** — selectable source (CV1-8 / TR1 / TR2), default **CV8** so both
trigs stay free as buttons. Detector threshold lowered (1500/800) so a
knob-attenuated CV clock and a full-swing trig both register. BPM + lock.

**Recording** — bar-quantized: arm -> starts on the next bar -> records
`bars` bars -> auto-stops to looping. Unsynced records at buffer length.
The bar fills toward the auto-stop during REC; lane frame is red (REC),
yellow (ARM), cyan (selected).

**Controls**
- TR1 = record/action (context cycle: arm / cancel / punch / stop / re-arm),
  TR2 = play/stop on the selected lane. Encoder mirrors: turn = select lane,
  press = action, hold = exit Live. A trig chosen as clock is masked out.
- CV6 = selected track level, CV7 = pan. CV1 = BP filter cutoff (rising CV
  opens the band), CV2 = resonance. (Knobs 6/7 are the two good ones on this
  unit; the filter jacks want an envelope/LFO patched in.)

**Per-track BP filter** — Setup toggle (default OFF). State-variable
bandpass; each track has its own cutoff/res/state (4 run at once). CV1/CV2
drive the selected track's cutoff/res, but only when actually driven, so each
track REMEMBERS its setting and an unpatched jack won't slam it on select.

**Setup page** — Sync / Clock Src / Bars / Monitor / BP Filter / Save Trk.
- Monitor: pass line-in through to the output.
- Save Trk: writes the selected track's loop to /sdcard/usr/LOOP_NNNN.RAW +
  .JSN (recording-service format), so it survives reboot and loads in the
  samplers. Shows SAVED / EMPTY.

**Live view** — 4 lanes, incremental (flicker-free) drawing: taller
transport bars, a thin sweeping playhead line, per-track volume meter, dark
blue backgrounds, control hint along the bottom.

RAM-only loops except via Save. Next ideas: overdub, per-track bar length,
downbeat alignment, direction toggle for the filter CV.

## Flash

```bash
./flash.sh                          # /dev/cu.usbserial-3110 by default
./flash.sh /dev/cu.usbserial-XXXX
```
