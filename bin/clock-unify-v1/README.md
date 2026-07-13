# clock-unify-v1 (2026-07-13)

Firmware `machines-20260713c` — the clock input UNIFICATION milestone, plus
the same-day hygiene flash.

- ONE clock input stack: every clock consumer (sampler3, deck, tracker,
  looper, glitch) goes through the shared `clockin_t` conditioned front-end
  (components/machine/clock.{h,c}) — floor-tracked Schmitt, ppb-scaled
  sanity gates, AC-tail ghost gate, raw-fire diagnostics. The private
  per-machine Schmitt copies and the raw fixed-threshold (1500/800) feeds
  are gone.
- Lock quality instrument-verified after the port (Scarlett A/B, same rig):
  deck pre −4.41 ms / 7.93 std → post −5.79 ms / 7.73 std; tracker post
  3.10 ms std, +0.36 ms/min drift.
- Hygiene: deck-analysis serial heartbeats stripped; /files/raw accepts
  suffixed names (double-.RAW 404 fixed).

Flash: ./flash.sh (esptool with --flash_size detect; port /dev/cu.usbserial-3110)
