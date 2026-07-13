# dualdeck-v1 (2026-07-13)

Firmware `machines-20260713j` — the DUAL-DECK machine, v1 COMPLETE
(core + loop stage), on top of streaming-slicer-v1.

- A clock-locked track BLENDER: two dk-style streaming decks off one reader,
  both PLL-locked to the shared clockin_t (beatmatched by construction),
  bar-quantized entries/exits, equal-power crossfade with takeover fade
  (held state + spike-immune grab), master DJ filter on knob7.
- UNIFIED TR GRAMMAR on the FOCUSED deck (Arlo): TR1 tap = quantized
  start/restart, hold = quantized stop; TR2 press = per-deck LOOP toggle
  (nearest-beat anchor, engine-owned wraps, ladder 1..16 beats via Setup),
  held = momentary. Encoder: turn = focus, press = load, long = Setup.
  Trade-off: trigs address the focused deck, not A/B fixed.
- Capture-verified: entries +8.4 ms deterministic, re-entry bar-coherent at
  26.000 bars, steady lock -5.3 ms / 2.16 std; loop wraps hold E±0, S0.
- Also fixes the single deck's latent 16-beat/slow-tempo loop-window ring
  eviction (ladder now ring-capped in both machines).

NOTE: shipped pre-hands-on at Arlo's cadence; gestures revisable after.

Flash: ./flash.sh (esptool with --flash_size detect; port /dev/cu.usbserial-3110)
