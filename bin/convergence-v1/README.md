# convergence-v1 (2026-07-13)

Firmware `machines-20260713e` — deck/tracker CONVERGENCE (S1-S8 of the
deck-tracker-convergence plan) on top of dualdeck-v1-core, looper-v3 and
clock-unify-v1 (all same-day).

- Shared trig_gate.h: one tap/hold grammar for every machine.
- UNIFIED TR GRAMMAR (deck + tracker): TR1 tap = play/pause, TR1
  hold-release = restart; TR2 press = loop toggle (engage on press),
  TR2 held = momentary loop. deck_sync_now unbound (grave marker).
- Tracker STEP-NUDGE: synced encoder shifts the row grid by whole clock
  pulses (sticks — invisible to the servo); 0.25 s synced render lead.
- Deck KO-II BUFFER LOOP: seamless beat-anchored engage (S5 lead cap
  keeps 1 s ring history), engine-owned whole-window wraps (PLL rides
  through, E±1 verified over multiple wraps), CV7 = length ladder
  1..16 beats, CV6 = whole-window start moves (seek protocol), knob
  freeze while looping, freeze/keeps-running release (phantom seek),
  loop-pink posbar band + LOOP info + Setup Loop Freeze row.
- Also aboard: DualDeck machine v1 core (bar-quantized entries verified
  +8.4 ms deterministic, takeover crossfade), looper house-style UI.

MIGRATION: deck TR1 tap no longer restarts (hold does); TR2 tap no
longer play/pauses (TR1 does). Tracker TR1 fires on release now.

Flash: ./flash.sh (esptool with --flash_size detect; port /dev/cu.usbserial-3110)
