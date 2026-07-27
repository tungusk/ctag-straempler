# tools/bench — analog capture rig

Records the module's **analog output** through an audio interface and drives it
over REST. Built 2026-07-26 while chasing an intermittent burst of noise that
none of the on-device meters could see.

Why analog rather than `/bounce` or the `:8000` broadcast: both of those put load
inside the thing under test. A bounce's SD writes alone pushed peak block cost to
1447 us of a 1450 us budget on a DRY case. This measures the module without
measuring the measurement.

## Setup, once

Patch BOTH module outputs into interface inputs 1 and 2, then:

    tools/bench/calib.py           # check the rig, write calib.json
    tools/bench/calib.py --verify  # confirm nothing has drifted since

Checks both channels carry signal, headroom, channel balance, the noise floor,
and the wall-clock-to-recording-time offset. **Do not touch the interface gain
afterwards** — `--verify` fails if it moves, which is the point.

## Use

    tools/bench/listen.py 90 out.wav    # capture while a human plays
    tools/bench/detect.py out.wav       # find discontinuities
    tools/bench/soak.py --hours 3       # unattended hunt, clips every hit
    tools/bench/soak.py --hours 3 --no-drive   # a human plays; we only listen

`soak.py` writes `hit_NNNN.wav` plus `hit_NNNN.json` (the events, the fitted
offset, and a `/status` snapshot) for segments containing something unexplained,
and deletes the rest.

## How the detector decides

A signal band-limited to f_max at amplitude A cannot slew faster than
`2*pi*f_max*A` per sample. Measured per capture from a high percentile of the
material's own slew, so the music defines its own ceiling; anything well above it
did not come from the signal. Validated against a labeled capture where Arlo
heard the artefact: four mid-note events at 0.476 / 0.265 / 0.109 / 0.037 while
the music never exceeded 0.029.

`soak.py` additionally discounts events caused by its OWN notes — the attack, and
the step when a non-looping sample runs out (Keys zeroes the voice with no fade
once the cursor passes the last frame). It fits the recording offset per segment
because sox's device-open time jitters 0.5-0.75 s run to run, and reports the fit
so it can be sanity-checked.

## Rules the code enforces, each learned the hard way

- **Check a take is not silent before analysing it.** Silence still yields a
  fundamental, a modulation depth and a click count, all garbage — one batch of
  conclusions came from captures at -66 dBFS where the analysis locked onto mains
  hum and reported it as a 60 Hz fundamental.
- **Window on the note, not the file.** Including the reverb tail filling read as
  49% modulation on a case whose settled value is 16%.
- **Pin f0 across a comparison set.** Detecting per take put one take at 115.7 Hz
  and made it look 96% inharmonic when it was simply playing flat.
- **Read parameters back after setting them.** `/remote/params` is a partial
  preset; anything omitted keeps whatever the last test left behind.
- **Run every condition twice.** A number that does not repeat is not a
  measurement.
- **Never poll `/status` while judging audio.** Polling at 4 Hz pushed peak block
  cost to 1495-1517 us and was audible. Every device meter is peak-hold and
  cleared on read, so one read after a segment covers the whole segment.
