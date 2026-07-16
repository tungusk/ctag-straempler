# slicer-fx-v1 — 2026-07-16

Full firmware snapshot, **OTA-capable layout** (two 3 MB app slots + otadata,
rollback-enabled bootloader). `./flash.sh [PORT]` serial-flashes it — the
**recovery path** if a bad OTA ever fails to boot. After this, updates are
WiFi: `tools/ota.sh`.

## Highlights since synth-v1

- **OTA + rollback (the churn-killer).** `POST /ota` streams firmware into the
  inactive slot over WiFi (`tools/ota.sh`, ~14 s, untethered); a bad image
  auto-reverts to the last good slot on the next reset. `GET /ota/state`.
- **Radio** station-change **wedge fixed** (two-task race → generation guard);
  press = play/stop; redraw throttle; SPAZ preset; `[ ]` markers; bigger name.
- **Radio `.m3u` import** + **tap-to-browse** file picker (iOS) in the web UI.
- **Looper** Live black background (deck grammar).
- **Slicer — big pass:**
  - **CV1 = quantized 1V/oct** pitch (×0.125..×2, tail-stream limited); CV2 = level.
  - **Fire crossfade** (declick on interrupting fire); draw-at-start fixed
    (redraw on the reader-load edge).
  - **Two-level BOXED encoder UI** (deck grammar): three screen-element boxes —
    transport (waveform, white center line, **green selected slice + peaks**),
    big file-name box (browser defaults to SLICES), and a drums-style **FX box**.
    Border thickness encodes the menu LEVEL (thin white browsing → thick white
    inside the transport). Encoder freed since CV6/TR1 already select/fire.
  - **FX chain: resonant SVF low-pass → Dattorro reverb.** FX box toggles it;
    **contextual knobs** (FX selected → knob6=cutoff / knob7=res); Reverb mode +
    Rev Mix in Setup; NaN self-heal on the filter.

## Recovery / layout note
This is the OTA layout: bootloader `0x1000` / partition-table `0x8000` /
ota_data `0xf000` / app `0x20000`. `bin/synth-v1` is the OLD single-app layout
(app @ 0x10000) — flashing it reverts OTA; re-migrate to get OTA back.

Matching git tag: `slicer-fx-v1-20260716`.
