# looper-v1 firmware (2026-07-05)

Snapshot of `v09-machines` at commit `755064d` (tag `looper-v1-20260705`).
Built with IDF 4.3 + xtensa-esp32-elf esp-2021r2-patch3-8.4.0.
**Approved by Arlo on hardware** — M2 looper, the first machine built entirely
on the machine architecture (validates it end-to-end with a non-sampler).

Machines (System -> Machine, choice persisted): Sampler / Sampler2 / Looper / Stub.

## Looper (M2)

Four-track clock-synced RAM looper.
- 4 mono tracks in PSRAM (8s each), mixed to stereo with per-track level + pan
- Clock on a selectable source (CV1-8 / TR1 / TR2; default TR1 — a trig is a
  clean gate edge, no attenuverter in the path). Rising-edge detector,
  8-interval ring-averaged period, 20-300 BPM sanity gate, BPM + lock readout
- Bar-quantized record: arm -> starts on the next bar (4 clock pulses) ->
  records `bars` bars -> auto-stops to looping playback. Records immediately at
  buffer length when unsynced
- Action cycle (encoder press or the non-clock trig): EMPTY->arm, ARMED->cancel,
  REC->punch out, PLAY->STOP, STOP->re-arm (record over — encoder-only redo)
- CV6 -> selected track level, CV7 -> pan (the two good knobs on this unit;
  focus-style, values persist per track). Encoder turn selects lane,
  hold exits Live
- Live 4-lane view (incremental draw, no flicker) + Setup (Sync/Src/Bars)

RAM-only in v1 — save-to-library and overdub are the next additions.

## Bringup bugs fixed (all this session)

active-low trig phantom self-record; menu-bar header clobber; bar-quantize
timing (was 4*bars, now every bar); redraw flicker; main-menu labels not
repainting on return from a full-screen machine page.

## Flash

```bash
./flash.sh                          # /dev/cu.usbserial-3110 by default
./flash.sh /dev/cu.usbserial-XXXX
```
