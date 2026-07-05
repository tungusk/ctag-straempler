# v09-dev-stable firmware (2026-07-03)

Snapshot of `v09-dev` at commit `bc01b4a` (tag `v09-dev-stable-20260703`).
Built with IDF 4.3 + xtensa-esp32-elf esp-2021r2-patch3-8.4.0. Hardware-verified.

Contains everything through the 2026-07-02/03 sessions:
- code-review fixes (autosave crash, current_bank overflow, codec R4 values, REST hardening)
- WiFi: PHY TX capped at 10 dBm (fixes the antenna-attached boot loop — RF cal
  burst was collapsing the rail), AP-fallback with 60 s STA retry, txpwr setting
- UI: sample browser list view (big-font selected name), 44 px numbered CV
  meters, dark-blue nav bar, taller transport bars, External In under Play,
  User-default sample loader, ±200% playback speed, Env On/Off ADSR bypass,
  Recording Monitor toggle (persisted), upload page with working URLs

## Flash

```bash
./flash.sh                          # uses /dev/cu.usbserial-3120 by default
./flash.sh /dev/cu.usbserial-3110   # or specify port
```
