# glitch-v1 firmware (2026-07-05)

Snapshot of `v09-machines` at commit `4a5163d` (tag `glitch-v1-20260705`).
Built with IDF 4.3 + xtensa-esp32-elf esp-2021r2-patch3-8.4.0. Hardware-approved.
Completes the machine roadmap — all seven machines in one firmware.

Machines (System -> Machine, persisted): Sampler / Sampler2 / Looper / Slicer /
Granular / Glitch / Stub. Each remembers its own settings (per-machine autosave).

## Glitch (M5)

Live-input stutter / beat-repeat. The only machine that works on line-in (no SD).

- 2 s stereo capture ring: passes audio through while continuously recording it.
- **TR1 held** (or **TR2** to latch, hands-free) freezes and loops the most
  recent window until released.
- **knob 6** = window length (20-500 ms), **knob 7** = pitch of the stutter
  (unity plateau at centre, 0.5x-2x), Setup **Reverse** toggle, CV1 jack = level.
- Live view: big LIVE / GLITCH state block + window/params.

Beat-sync (window as a clock division, reusing the looper's clock detection) is
the natural follow-up.

## Flash

```bash
./flash.sh                          # /dev/cu.usbserial-3110 by default
./flash.sh /dev/cu.usbserial-XXXX
```
