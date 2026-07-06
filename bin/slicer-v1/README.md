# slicer-v1 firmware (2026-07-05)

Snapshot of `v09-machines` at commit `61f00ab` (tag `slicer-v1-20260705`).
Built with IDF 4.3 + xtensa-esp32-elf esp-2021r2-patch3-8.4.0. Hardware-tested.
**Approved by Arlo** — the M3 slicer, fifth machine.

Machines (System -> Machine, persisted): Sampler / Sampler2 / Looper / Slicer / Stub.
Each remembers its own settings independently (per-machine autosave, keyed by
name in AUTOSAVE.JSN).

## Slicer (M3)

One stereo sample from the SD library (usr/*.RAW, e.g. loops saved by the
looper) loaded into PSRAM (<=12s), chopped into slices; one monophonic
retrigger voice. All controls act on this single track.

- **Slice modes**: Grid (equal 8/16/32) or Transient (onset detection). Slices
  is Auto/8/16/32; in transient+Auto the count follows the material.
- **Sensitivity screen** (Setup -> Sensitivity): waveform + live slice grid,
  turn the encoder to re-slice — threshold is relative to the loudest onset so
  the count sweeps from ~1 slice up to the ~80 ms spacing limit. Forces
  transient+Auto. Energy envelope cached at load so the dial is responsive.
- **Controls**: knob 6 = slice select (encoder also selects), knob 7 = pitch
  (unity plateau at center, 0.5x-2x), CV1 jack = level, TR1 = fire selected
  slice, TR2 = fire + step. Reverse + Auto-advance toggles.
- **Sample browser** (Setup -> Sample): centered scroll list, big selected
  name, 4 neighbours each side, press to load / hold to cancel.
- **Live view**: waveform + slice grid (non-uniform in transient), selected
  (cyan) + playing (green) slice highlighted, incremental redraw; sample/grid/
  slice info below the waveform.

No record mode by design — bring samples in via the looper's save-to-library
or uploads. Loads directly into PSRAM (factor into a shared sample_ram service
when granular/glitch arrive).

## Flash

```bash
./flash.sh                          # /dev/cu.usbserial-3110 by default
./flash.sh /dev/cu.usbserial-XXXX
```
