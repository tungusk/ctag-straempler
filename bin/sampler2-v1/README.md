# sampler2-v1 firmware (2026-07-05)

Snapshot of `v09-machines` at commit `68d4463` (tag `sampler2-v1-20260705`).
Built with IDF 4.3 + xtensa-esp32-elf esp-2021r2-patch3-8.4.0.
**Approved by Arlo on hardware** — this is the Sampler2 reference build and
the groundwork baseline for future machines (M2 looper next).

Machines: Sampler (untouched runtime fallback) / Sampler2 / Stub, switchable
via System → Machine, choice persisted in CONFIG.JSN.

Sampler2 over the classic sampler:
- CROP play mode: window = start + length; Start slides the window (menu+CV),
  the Loop End row/dest acts as LENGTH in crop ("L n %"); set values are home,
  CV breathes around them; ~100 ms minimum loop guard (SD protection)
- Signed matrix amounts (-100..100, negative inverts response); CV1/2 floor
  trim (1V/oct jacks idle ~21%, now reach true 0 as mod sources); full-depth
  playback-point modulation (amt × cv × filelength, no headroom traps)
- CV rows 1/2 fully reassignable (pitch keyed by destination); CV meter
  mapping labels; voices display as 1/2 everywhere
- Encoder acceleration on loop points, playback speed, filter base/width,
  delay time; quadrature decode = one event per detent (core)
- Load flow: main-menu "Load" → voice → User list directly; slot rows in
  DejaVu18; voice 2's browser right-justified; long-press escapes browsers
  without loading; Freesound hidden (future machine)

Unit-specific corrections baked in (core): no 5/6 ADC swap, no ch3/4
inversion (upstream PCB compensations this board doesn't have), T-ARM header
labels hidden, "More" menu is "System".

Known-open hardware: CV4 jack broken (reads pinned high) — Arlo tracking
down; knobs 5/8 attenuate patched CV but cap ~half level; CV3/4 are bipolar
(±5V) inputs, unipolar sources reach only ~40%-down.

## Flash

```bash
./flash.sh                          # uses /dev/cu.usbserial-3110 by default
./flash.sh /dev/cu.usbserial-XXXX   # or specify port
```
