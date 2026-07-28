#!/usr/bin/env python3
"""Do the REST requests themselves cause the glitches?

On a continuous tone the detected events sit at multiples of ~1.33 s — exactly
the interval at which this harness posts /remote/trig to hold the gate open.
That would mean the rig has been generating the artefact it is measuring, which
also explains the gate-length result (same number of posts either way, but a
longer note leaves a longer window in which a post-induced glitch is audible).

Same sound throughout; only the POST RATE changes. The gate endpoint caps at
2000 ms, so anything under ~1.9 s sustains it — the sound is identical whether
we refresh every 1.9 s or every 0.5 s, but the network traffic differs 4x.

If events scale with the post rate, the harness is the cause. If they hold
steady, the module is.
"""
import time

import numpy as np

from detect import find_bursts
from rig import Rig, load_wav, post

SECS = 30.0
BLOCKS = 4                      # per condition, interleaved
RATES = [1.9, 0.5]              # seconds between gate refreshes

rig = Rig()
keys_patch = rig.params()
SY = {"eng": 0, "note": 48, "quant": True, "shape": 0.0, "atk": 0.01, "dec": 0.05,
      "sus": 1.0, "rel": 0.2, "e2c": 0.0, "gld": 0, "cut": 900.0, "res": 0.2,
      "fold": 0.0, "lvl": 0.8, "fxsl": [2, 0], "rv": 2}

res = {r: {"events": 0, "secs": 0.0, "posts": 0} for r in RATES}
try:
    post("/remote/machine?name=Synth")
    time.sleep(4)
    if rig.status().get("machine") != "Synth":
        raise SystemExit("could not switch to Synth")
    rig.set_params(SY, verify=False)
    time.sleep(1.5)

    for blk in range(BLOCKS * len(RATES)):
        rate = RATES[blk % len(RATES)]
        wav = "posts_%s.wav" % str(rate).replace(".", "")
        proc = rig.capture(SECS, wav)
        time.sleep(0.6)
        t0 = time.time()
        n = 0
        while time.time() - t0 < SECS - 1.0:
            rig.trigger(2000)
            n += 1
            time.sleep(rate)
        try:
            proc.wait(timeout=SECS + 20)
        except Exception:                              # noqa: BLE001
            proc.kill(); time.sleep(1); continue
        x, sr = load_wav(wav)
        ev, thr, ceil = find_bursts(x, sr)
        res[rate]["events"] += len(ev)
        res[rate]["secs"] += SECS
        res[rate]["posts"] += n
        print("  block %d  refresh %.1fs  %d posts  %d events  (peak %.2f)"
              % (blk, rate, n, len(ev), float(np.max(np.abs(x)))), flush=True)
finally:
    post("/remote/machine?name=Keys")
    time.sleep(4)
    rig.set_params(keys_patch, verify=False)
    print("\nback on Keys (smp %r)" % rig.params().get("smp"))

print("\n=== RESULT ===")
print("%-10s %-8s %-8s %-10s %-12s %s"
      % ("refresh", "posts", "events", "ev/sec", "ev/post", "sound"))
for r in RATES:
    d = res[r]
    print("%-10.1f %-8d %-8d %-10.3f %-12.3f %.0f s"
          % (r, d["posts"], d["events"], d["events"] / max(d["secs"], 1),
             d["events"] / max(d["posts"], 1), d["secs"]))
a, b = res[RATES[0]], res[RATES[1]]
ra = a["events"] / max(a["secs"], 1)
rb = b["events"] / max(b["secs"], 1)
pr = (b["posts"] / max(b["secs"], 1)) / max(a["posts"] / max(a["secs"], 1), 1e-9)
print("\npost rate ratio %.1fx   event rate ratio %.1fx" % (pr, rb / max(ra, 1e-9)))
print("-> %s" % ("THE HARNESS: events scale with REST traffic"
                 if rb > 1.8 * ra else
                 "not the posts: event rate is independent of REST traffic"))
