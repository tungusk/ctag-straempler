# tape-fxchain-v1 (2026-07-26)

Full firmware snapshot at `tape-fxchain-v1-20260726`. Continues `tape-drop-v1`
(2026-07-25), which archived only the first commit of that session — this covers
the five that followed.

Serial-flash (recovery only — normal updates are `tools/ota.sh <IP>`):

    ./flash.sh [/dev/cu.usbserial-XXXX]

OTA layout (app @ 0x20000). Flashing an OLD single-app archive (e.g.
`bin/synth-v1`) reverts OTA and needs a re-migration.

## What's in it

**Tape FX ROUTE — pre / post / off** (Setup row + Live button)
- `pre` prints the rack to tape (the original behaviour); `post` colours the
  OUTPUT including playback and records dry; `off` bypasses the rack.
- `post` LATCHES to `pre` on the first punch-in: audition effects over a dry
  take, and whatever you were hearing starts being printed the moment you
  commit. (Began as a separate AUTO mode; folded into `post` as redundant.)
- Tape's own filter+drive is an INPUT-path preamp and deliberately does NOT
  follow the route. Routing it onto playback ran drive over the take a second
  time (~4x gain at drive 0.5 — "volume seems to double"); folding it into the
  bypass made `off` dump ~10 dB. It is always printed, in all three modes.

**PSRAM recovery — the pool is 4.00 MB and that is a HARDWARE ceiling** (the
ESP32 maps at most 4 MB of external RAM). A 60 s tape at 5.05 MB was never
reachable and silently fail-softed. Two fixes, both measured on hardware:
- `fxrack` now FREES lazy slabs no slot uses (`fxdelay` ~690 KB, `flanger`
  ~90 KB). They were allocated on first use and never released, so a delay
  auditioned ONCE squatted on 690 KB for the rest of the session — capping the
  tape at ~32 s and leaving 0.17 MB free, at which point other lazy allocations
  fail silently. That was the reported "hurt the fx audio quality".
- Tape banks cut 1 MiB -> 128 KB, recovering the 0.38 MB that bank rounding
  discarded (a 30 s tape is 2.52 MB of audio but rounded to 3 whole banks).
- Result: **3.62 MB available to the tape = 43 s = 72 beats at 100 bpm.**
  Length options are now 15/30/**40** s. Caveat: at 40 s there is no room for
  the delay slab — long tape OR a delay, not both.

**Clicks — measured, not guessed.** `/status` gained `auspk`, a PEAK-HOLD of the
machine `process()` cost cleared on each read; `aus` is an EMA and smooths the
single overrunning block that a click actually IS.
- *Reverb mode change* (ear-confirmed fixed): measured **2282 us against a
  1450 us budget**. Never the reverb's signal — a 20 ms return fade changed
  nothing, which was the clue. `tank_resize` memset 124 KB as one unchunked
  burst and the clear chunks were 32 KB = 2.1 ms, longer than a whole audio
  block. `tank_resize` no longer clears; chunks are 8 KB (~0.5 ms).
- *FX button presses* repainted the whole 300-column waveform (`draw_wave`),
  the known audio-starving redraw — an effect swap does not change the
  waveform. Strip-only now.
- *Flanger into overdrive/reverb sounded overdriven*: NOT cpu (aus 678 of 1450)
  and NOT output clipping (VU 98 of 255) — gain staging between stages. The ring
  accumulates toward x/(1-fb), so raising feedback also raised LEVEL into the
  next stage. Compensated with 1/(1+|fb|) on the stage output. NOTE the ring
  clamp stays at 60000: clamping it at full scale hard-clips inside the feedback
  loop on ordinary material (tried it, Arlo caught it immediately).
- *Shimmer behind a flanger*: the feedback injection had no bound at all (`sg`
  is tuned for a flat source; a resonant one raises effective loop gain) — now
  ceilinged at half scale, exactly linear below the knee. And the grain
  crossfade was a bare triangle whose DERIVATIVE kinks at the apex/wrap,
  splattering at the ~21.5 Hz grain rate — now smoothstepped. Residual
  graininess is structural (two heads 46 ms apart genuinely disagree on a swept
  comb); Arlo: "tolerable, corner case correct".
- **Overdrive trimmed 6 dB** inside the stage — `tanh` saturates to +/-1 so it
  sat near full scale whatever went in. Trimmed there rather than lowering the
  default so EXISTING patches come down too. **This is shared: Keys / Synth /
  Drums overdrive is quieter too, and has NOT been ear-tested on those.**

**POST /reboot + OTA self-recovery.** A long flash session fragments internal
RAM until `esp_ota_begin` cannot get its contiguous buffer (hit twice at ~34 KB
largest block); recovery used to mean pulling power. `/reboot` reuses the OTA
shutdown sequence (autosave, mute, restart) and REFUSES while recording.
`tools/ota.sh` now soft-reboots and retries once on that specific failure.
It cannot rescue a WEDGED device — if httpd is starved, nothing answers.

**Other:** OTA mutes the output for the whole flash; `tape_load` mutes the
monitor while it streams; header split into independently-repainted status/name
bands (the name was being erased 10x/s during playback — the "flashing
filename"); button strip is a 2-row layout table sized for growth; FX buttons
show the effect name, falling back to the slot number only when empty; browser
opens in `usr/TAPE`; `tapelast` persists whenever the take's identity changes,
not only on a graceful leave; `/sysinfo` reports psram + iram free/largest.

`plans/tape-streaming-20260725.md` — design for breaking the length ceiling
properly (window in RAM, tail streamed from card). NOT started.

## Verification

Built clean; `tools/proof_build.sh` passes. Archived binary sha256 matches
`build/`. Ear-confirmed by Arlo: reverb click resolved, flanger+overdrive good,
flanger+reverb clean, shimmer tolerable, record/save/crop working, 40 s tape and
the slab GC working.

**EAR-TESTS OWED:** the overdrive trim on Keys/Synth/Drums (6 dB quieter as a
side effect of a Tape fix, unheard on those machines); `post` vs `pre` printing
with Drive engaged; the crop's contents on a monitor; the overwrite detector's
amber path. Also still queued: the sample browser lists the CURRENT folder as a
row inside itself, and other clocked machines have not been audited for the
CV8-style clock/knob collision fixed in Tape.
