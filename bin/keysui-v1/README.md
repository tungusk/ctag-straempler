# keysui-v1 (2026-07-18)

Flashable snapshot. OTA-capable layout (ota_0/ota_1 + otadata). WiFi update:
`tools/ota.sh <IP>`. Serial recovery: `bin/keysui-v1/flash.sh [PORT]`.

## What's in this milestone (on top of fx-stability-v1)
A Keys + Synth **Live-screen polish pass**, plus the audio fixes that fell out of
ear-testing it. All UI changes are shared-shape (Keys `isampler_menu.c`, Synth
`synth_menu.c`); the audio fixes are in the shared reverb (`util/reverb.c`).

### Live UI
- **Bigger knob dials**, value **centred in the ring**, pointer that **escapes
  the circle** (a rim tick, not a hub spoke), labels stay below. Dials recentred
  + row nudged down so the escaping needle clears the waveform / osc strip.
- **ADSR curve dims** when it isn't the encoder focus and brightens when it is;
  the **selection dot is bigger and green** (was a small pale dot), clamped
  inside its box so the Release point at the corner leaves no artifact.
- **Keys waveform** dims when the Sample element isn't focused + a **white
  zero/origin line** across the strip (continuous under the playhead sweep).
- **Green edit/active highlights everywhere** (dials, ADSR dot, osc trace,
  header tag) — replaced the amber/yellow.

### Audio fixes (found by ear on the Live screen)
- **Redraw throttle** — the heavy element redraws (dials/osc/waveform/ADSR) are
  gated to the 1 Hz timer; only the cheap playhead + note readout stay on the
  300 ms timer. Knob/CV ADC jitter had been redrawing the (now bigger) dials
  ~4×/s, and those TFT + PSRAM shadow-framebuffer writes contended with the
  PSRAM audio path → noise then dropout. Fixed.
- **Shimmer reverb no longer fades to silence over time.** A DC offset was
  circulating the shimmer feedback loop and slowly railing the tank (the "fades
  all the way out / needs a reset" drift). The shimmer return is now DC-blocked
  (`shim_dc`) as well as low-passed + gain-limited (fx-stability-v1).
- **Reverb mode switch no longer explodes.** `reverb_set_mode` now mutes the
  tank (mode → OFF) and clears every delay line before engaging the new mode, so
  there's no residual tail to detonate under the new decay/shimmer gain. (Small
  click as the old tail cuts is expected.)

## NOT changed
Other machines' Live screens, the FX DSP other than the two reverb fixes, the
redraw cadence off the Live screens. Broadcast still OFF by default.
