# granular-v1 firmware (2026-07-05)

Snapshot of `v09-machines` at commit `f343f3b` (tag `granular-v1-20260705`).
Built with IDF 4.3 + xtensa-esp32-elf esp-2021r2-patch3-8.4.0. Hardware-tested,
sounds great; 16 grains of float DSP fit the CPU budget with headroom.

Machines (System -> Machine, persisted): Sampler / Sampler2 / Looper / Slicer /
Granular / Stub. Each remembers its own settings (per-machine autosave).

## Granular (M4)

Grain-cloud playback of a mono sample from the SD library (usr/*.RAW) loaded
into PSRAM (<=12s).

- **16-grain pool**; each grain is a raised-cosine-windowed (Hann LUT),
  linear-interpolated read from a position in the sample, panned per-grain
  for stereo spread.
- **Controls**: knob 6 = cloud position, knob 7 = pitch (unity plateau at
  centre, 0.5x-2x), CV1 jack = level, TR1 held = freeze the position.
- **Setup**: Grain ms (10-500), Density (grains/sec), Spray (position jitter),
  Spread (pan width), Sample (centered browser).
- **Live view**: waveform + moving cloud-position marker + live grain count.

Loads mono into PSRAM directly (factor into a shared sample_ram service when
glitch/more machines arrive). Bring samples in via the looper's
save-to-library or uploads.

## Flash

```bash
./flash.sh                          # /dev/cu.usbserial-3110 by default
./flash.sh /dev/cu.usbserial-XXXX
```
