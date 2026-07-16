# keys-v1 — 2026-07-16

Full firmware snapshot, **OTA-capable layout** (two 3 MB app slots + otadata,
rollback-enabled bootloader). `./flash.sh [PORT]` serial-flashes it — the
**recovery path** if a bad OTA ever fails to boot. After this, updates are
WiFi: `tools/ota.sh`.

## Highlights since menu-unify-v1

- **NEW MACHINE — "Keys", a Tonal Instrument Sampler** (`machine_instsampler`,
  16th in the registry). A pitched, playable sampler built on the Synth's
  operability grammar with a real varispeed sample voice + sustain loop in place
  of the oscillator:
  - One PSRAM-resident mono sample (≤~22 s), varispeed-pitched from **CV1 at
    1V/oct** (the measured 877/49 scale) with **cubic interpolation**, ±2 octaves.
  - **Forward sustain loop** so held notes never run out — the loop wraps while
    the note is held, with a **zero-crossing snap** on the loop points and an
    **equal-power wrap crossfade**.
  - The Synth's linear **ADSR** + env-opened **SVF** (per-voice, NaN self-heal),
    four **takeover macro knobs** (K5 start / K6 cut / K7 res / K8 env>cut), and
    an **8-destination CV matrix** (cutoff / res / env>cut / level / pitch /
    start / loopmov / looplen). Optional Dattorro reverb.
  - Live dashboard: waveform strip with the loop window drawn as a box + a white
    playhead, four macro dials, ADSR curve. Setup on the shared setup_menu
    framework; sample browser; CV matrix page.
  - State factored into voice[]/zone[] arrays so #28 paraphony and v2 multi-zone
    are additive. process() reads PSRAM only; a loading gate keeps it silent
    during a sample swap.
- **Synth named patch save/load (#23):** per-patch JSON at `usr/synth/PAT_NNN.jsn`
  (auto-numbered, 8.3-safe); Setup rows Save Patch + Load Patch (newest-first
  browser).

Status: build + `tools/proof_build.sh` pass; OTA-flashed (ota_0); Keys
smoke-tested (switches in/out cleanly, silent with no sample). **The sampler's
audio/loop/pitch behaviour is NOT yet ear-tested** — needs a patched gate + CV
and a loaded sample. Named-patch recall for Keys is a follow-up (mirrors #23).

## Recovery / layout note
OTA layout: bootloader `0x1000` / partition-table `0x8000` / ota_data `0xf000`
/ app `0x20000`. `bin/synth-v1` is the OLD single-app layout (app @ 0x10000) —
flashing it reverts OTA; re-migrate to get OTA back.

Matching git tag: `keys-v1-20260716`.
