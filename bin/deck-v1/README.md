# deck-v1

The Deck milestone: tempo-syncing track player, hardware-verified and
performance-approved 2026-07-08 ("the track deck is tight"). Snapshot of
commit `b4f9b53` (tag `deck-v1.1-20260708`; originally cut at `83d738d`,
refreshed same night with the final touch-ups below). Flash with
`./flash.sh [PORT]` (default `/dev/cu.usbserial-3110`, `--flash_size detect`).

## v1.1 refresh (same night)

- **Auto BPM** Setup toggle: auto-analyze-on-load can be disabled (persisted;
  cached-bpm tracks never re-analyze automatically either way).
- Hint line fixed: transport-bar paint leaked its color through the shared
  `_bg` TFT global, turning the bottom hint blue-on-blue.
- Analysis stays deliberately slow (~16 KB per SD burst with a 10 ms gap):
  full-stride reads audibly starve the SD bus even with the transport
  stopped — tried and reverted, don't re-optimize.

## New since drums-remote-v1

- **Deck machine**: streams long usr/*.RAW tracks from SD through an 8 s
  PSRAM ring (reader task; process() never touches SD), varispeed playback
  phase-locked to an external CV clock via a pulse-level PLL with clock
  mult/div (1/4..4 pulses per beat, floor-tracked Schmitt conditioning so
  attenuated/offset clock channels work). TR1 = restart at the downbeat,
  TR2 = stop, encoder press = track browser, turn = bar-snapped scrub,
  knob6 = DJ LP/HP filter, knob7 = x0.5/x1/x2 synced speed (free rate when
  sync is off).
- **Auto-analysis**: loading an unanalyzed track detects BPM + beat grid in
  the background during playback (onset-flux autocorrelation), shows an<n>%
  on the Live page, and caches "bpm"/"grid" in the track's JSN sidecar —
  one-time cost per track. Manual re-run in Setup.
- **Live page**: big target-tempo readout (green when locked), full-width
  position bar with green(play)/blue(stop) background, change-driven
  redraws throughout.
- **Diagnostics**: /status v1 line `P|s e<edges> i<lastms> p<periodms>
  E<phase%> S<ring-starves>`. Healthy = i==p, E near 0, S flat. Deck audio
  legitimately sounds rough ~60 s after boot (ring refill + PLL cold
  relock) and ~15 s after scrubs — judge audio only after settling.
- **rest-api fix**: an aborted /files/raw download no longer wedges the
  web server for minutes while hammering the SD bus.
- Since drums-remote-v1 also: boot_target (boot straight into a machine's
  live page), settings.hostname, custom /bootlogo + make_bootlogo.sh.
