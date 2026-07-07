# drums-remote-v1

The full next-machines milestone, hardware-verified 2026-07-06. Flash with
`./flash.sh [PORT]` (default `/dev/cu.usbserial-3110`, `--flash_size detect`).

## New since sd-hardening-v1

- **Drums machine**: 8 one-shot mono RAM pads. Direct mode with per-pad
  routable CV triggers (floor-tracking Schmitt + Low/Med/High Sensi),
  CV-select mode (TRIG1/2 + selector CV), per-pad level/pan/decay/enable,
  declick + retrigger fades, 2x2 grid in 4-voice mode, R/G/B corner-dot flash.
- **Freesound machine**: web-driven search/preview download + direct MP3-URL
  import into usr/ (44.1 kHz only; TLS cert bundle fix — https was silently
  broken since IDF 4.3).
- **Universal upload**: the web Upload tab converts any audio file in-browser
  to 44.1 kHz stereo RAW.
- **Teleremote (always-on core)**: Remote web tab — live CV/gate monitor,
  encoder control, soft trigger pulses, machine switching, and a machine
  settings editor (dedicated pad UI for Drums). Gated by the new
  System→Settings→Remote toggle (settings.remote in CONFIG.JSN).
- Machine web-URI hook (/fs/* served only while Freesound is active),
  sorted shared sample browser list (fixes the 32-file cap that hid new
  uploads), sd_lock hardening in mp3 decode + freesound download.
