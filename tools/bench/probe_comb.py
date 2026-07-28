#!/usr/bin/env python3
"""Is the artefact the MODULE, or the capture path's clock drift?

The events occur at ~0.1 per second of audible sound, uniformly — about one
every 10 s. The capture path has its own artefact every ~10.14 s (roughly 2 ppm
of drift between the interface and the Mac, corrected by dropping or inserting a
sample). That is close enough to demand a decision.

Discriminator: record a CONTINUOUS tone, so sound is present at all times and the
note structure cannot shape the result, then look at the spacing of detected
events. Clock drift is a metronome — its events land on a fixed comb. A fault in
the module has no reason to.

Uses Synth: it sustains indefinitely, needs no sample, and has no loop seam.
"""
import time

import numpy as np

from detect import find_bursts
from rig import Rig, load_wav

# 30 s chunks, not one long capture: sox wedges on long captures on this
# interface (seen twice — a 120 s capture hung immediately, while 30 s segments
# survived ~80 to 1). A 10.14 s comb still shows up as ~2 gaps per chunk.
SECS = 30.0
RUNS = 8

rig = Rig()
keys_patch = rig.params()
st = rig.status()
print("was on %s; switching to Synth for a continuous tone" % st.get("machine"))

SY = {"eng": 0, "note": 48, "quant": True, "shape": 0.0, "atk": 0.01, "dec": 0.05,
      "sus": 1.0, "rel": 0.2, "e2c": 0.0, "gld": 0, "cut": 900.0, "res": 0.2,
      "fold": 0.0, "lvl": 0.8, "fxsl": [2, 0], "rv": 2}

all_times = []
try:
    from rig import post
    post("/remote/machine?name=Synth")
    time.sleep(4)
    if rig.status().get("machine") != "Synth":
        raise SystemExit("could not switch to Synth")
    rig.set_params(SY, verify=False)
    time.sleep(1.5)

    for run in range(RUNS):
        wav = "comb_%d.wav" % run
        proc = rig.capture(SECS, wav)
        time.sleep(0.6)
        t0 = time.time()
        # hold the gate for the whole capture
        while time.time() - t0 < SECS - 1.0:
            rig.trigger(2000)
            time.sleep(1.2)
        try:
            proc.wait(timeout=SECS + 20)
        except Exception:                              # noqa: BLE001
            proc.kill()
            time.sleep(1)
            print("run %d: sox wedged, skipped" % run)
            continue
        x, sr = load_wav(wav)
        pk = float(np.max(np.abs(x)))
        ev, thr, ceil = find_bursts(x, sr)
        ts = np.array([e["t"] for e in ev])
        all_times.append(ts)
        gaps = np.diff(ts)
        print("run %d: peak %.3f, ceiling %.4f, %d events in %.0f s (%.3f/s)"
              % (run, pk, ceil, len(ts), SECS, len(ts) / SECS))
        if len(gaps):
            print("   gaps: %s" % " ".join("%.2f" % g for g in gaps[:14]))
            print("   gap median %.2f s   stdev %.2f" % (np.median(gaps), gaps.std()))
finally:
    post("/remote/machine?name=Keys")
    time.sleep(4)
    rig.set_params(keys_patch, verify=False)
    print("\nback on Keys, patch restored (smp %r)" % rig.params().get("smp"))

print("\n=== VERDICT ===")
gaps = np.concatenate([np.diff(t) for t in all_times if len(t) > 1])
if len(gaps) < 3:
    print("too few events to judge")
else:
    print("%d gaps: median %.2f s, stdev %.2f" % (len(gaps), np.median(gaps), gaps.std()))
    # how well do the gaps fit a fixed 10.14 s metronome (allowing missed beats)?
    per = 10.14
    resid = np.abs(gaps / per - np.round(gaps / per)) * per
    print("residual against a %.2f s comb: median %.2f s" % (per, np.median(resid)))
    if np.median(resid) < 0.35:
        print("-> LANDS ON THE COMB: this is the capture path's clock drift, not the module")
    else:
        print("-> does NOT fit the comb: the events are the module's own")
