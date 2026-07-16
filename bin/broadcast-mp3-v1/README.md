# broadcast-mp3-v1 — 2026-07-16

Full firmware snapshot, **OTA-capable layout**. `./flash.sh [PORT]`
serial-flashes it (recovery path); normal updates via `tools/ota.sh`.

## Highlights since keys-patch-v1

- **#17: live MP3 broadcast** — `http://<ip>:8000/live.mp3` streams the output
  bus as 96 kbps MONO 44.1k MP3 (icecast-style headers; VLC/browser/internet).
  Plain `:8000/` stays the stereo WAV LAN stream. Encoder = vendored **shine
  3.1.1** (`components/shine`, LGPL, state in PSRAM, -O3), spun up per-listener.
- `/bcast/state` now reports `diag` (last MP3-path error) + `enc_us` (smoothed
  shine cost per 26.1 ms pass — realtime needs < 26100).
- Web BROADCAST card: MP3 + WAV links; inline player uses MP3.

Measured (30 s captures, decoded clean at full rate): idle ~18 ms/pass, Deck
playing ~13 ms = realtime. **Radio playing ~39 ms = NOT realtime** (helix +
shine thrash the PSRAM cache) — re-broadcasting Radio drops audio; use the
LAN WAV stream or a bounce for that. Also learned: internal heap while radio
plays is near zero (4.6 KB malloc failed, OTA POST returns "oom") — stop
radio before OTA.
