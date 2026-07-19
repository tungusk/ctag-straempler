# fx-stability-v1 (2026-07-18)

Flashable snapshot. OTA-capable layout (ota_0/ota_1 + otadata). WiFi update:
`tools/ota.sh <IP>`. Serial recovery: `bin/fx-stability-v1/flash.sh [PORT]`.

## What's in this milestone (on top of tracker-ram-v1)
Two shared-FX feedback-stability fixes (`components/util/reverb.c`,
`components/util/flanger.c`) — they benefit every host (Keys / Synth / Tape /
Drums), found and verified instrument-first by capturing the `:8000` output
stream and measuring the RMS/brightness envelope over time.

### Shimmer reverb: runaway on silence — FIXED
The shimmer feeds an octave-up copy of the tank's own tail back into the tank.
The octave shifter doubles frequency every pass, so with no loss in that loop
the tail **bloomed upward until it pinned at full-scale and stayed there on
silence** (measured: after a note released, RMS climbed monotonically to 32722
and held). Two brakes:
- **`SHIM_FB_LP` one-pole low-pass on the shimmer return** — once energy climbs
  past the cutoff it's removed, killing the upward runaway.
- **Loop gain cut below 1** for the low/mid band the LP can't catch:
  `shim_gain` 0.45 → 0.38, tank `decay` 0.65 → 0.63.

Verified: a note now **blooms** (RMS ~2.4k, brightness rising = the octave sheen)
then **decays smoothly to silence** in ~5 s. No runaway; other reverb modes
(Room/Hall/Plate, `shim_gain`=0) are untouched.

### Flanger: harsh on held tones — softened
The feedback path already had a one-pole damping LP that (verified) keeps it
bounded even at feedback 0.95 — so it was never a runaway. Darkened the
recirculation damping (`dc` 0.5 → 0.38) so a sustained tone rings less metallic;
the wet output tap stays bright. Measured lower RMS + HF in sustain, still
bounded, still clearly a flanger.

## NOT changed
Non-shimmer reverb modes, the flanger wet tap, and every other FX. No new
persistent RAM. Broadcast stays OFF by default (tracker-ram-v1).
