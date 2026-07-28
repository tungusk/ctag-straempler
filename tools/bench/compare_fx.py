#!/usr/bin/env python3
"""Is flanger + reverb genuinely worse than either alone, or is it my traffic?

Normalises to events per second OF SOUND, not per second of capture: a note is
only ~2.5 s long, so a rate per capture-second mostly measures duty cycle. It
also measures its own floor in the same session — `dry` runs the identical note
and the identical number of REST posts with no FX at all, so anything the
harness causes appears there too and can be subtracted by eye.

Conditions are INTERLEAVED block by block, so drift (temperature, wifi, PSRAM
state) cannot masquerade as a difference between them.

  tools/bench/compare_fx.py [minutes]
"""
import sys
import time

import numpy as np

from detect import find_bursts
from rig import Rig, load_wav

MINUTES = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
SEG = 24.0
NOTE_EVERY = 3.0

CASES = [("dry",      {"fxsl": [0, 0], "rv": 0}),
         ("hall",     {"fxsl": [0, 0], "rv": 2}),
         ("flanger",  {"fxsl": [2, 0], "rv": 0}),
         ("flg+hall", {"fxsl": [2, 0], "rv": 2})]

rig = Rig()
base = rig.params()
print("patch: smp %r  rvmx %s  lvl %.2f  loop %s"
      % (base.get("smp"), base.get("rvmx"), base.get("lvl", 0), base.get("lm")))
print("%.0f min, %.0f s blocks, interleaved\n" % (MINUTES, SEG))

acc = {n: {"ev": 0, "snd": 0.0, "posts": 0, "ceil": []} for n, _ in CASES}
t_end = time.time() + MINUTES * 60
blk = 0
try:
    while time.time() < t_end:
        name, over = CASES[blk % len(CASES)]
        blk += 1
        rig.set_params(dict(base, **over), verify=False)
        time.sleep(1.0)
        wav = "cmp_%s.wav" % name.replace("+", "_")
        # notes fired from a thread so the capture call can block
        import threading
        stop = threading.Event()
        posts = [0]

        def notes():
            while not stop.is_set():
                try:
                    rig.trigger(1500)
                    posts[0] += 1
                except Exception:                     # noqa: BLE001
                    pass
                stop.wait(NOTE_EVERY)
        th = threading.Thread(target=notes, daemon=True)
        th.start()
        try:
            rig.capture_blocking(SEG, wav)
        except Exception as exc:                      # noqa: BLE001
            stop.set(); th.join(timeout=3)
            print("  capture failed: %s" % exc)
            break
        stop.set(); th.join(timeout=3)

        x, sr = load_wav(wav)
        m = np.abs(x).mean(axis=1)
        k = int(0.25 * sr)
        env = np.sqrt(np.mean((m[:len(m) // k * k].reshape(-1, k)) ** 2, axis=1))
        snd = 0.25 * float(np.sum(env > 0.01))
        ev, thr, ceil = find_bursts(x, sr)
        acc[name]["ev"] += len(ev)
        acc[name]["snd"] += snd
        acc[name]["posts"] += posts[0]
        acc[name]["ceil"].append(ceil)
        if blk % 4 == 0:
            print("  %d blocks — " % blk + "  ".join(
                "%s %.2f/s" % (n, acc[n]["ev"] / max(acc[n]["snd"], 0.1)) for n, _ in CASES),
                flush=True)
finally:
    rig.set_params(base, verify=False)

print("\n=== events per second OF SOUND ===")
print("%-10s %-8s %-9s %-9s %-9s %s" % ("case", "events", "sound s", "ev/s", "ceiling", "posts"))
for n, _ in CASES:
    d = acc[n]
    print("%-10s %-8d %-9.1f %-9.3f %-9.4f %d"
          % (n, d["ev"], d["snd"], d["ev"] / max(d["snd"], 0.1),
             float(np.mean(d["ceil"])) if d["ceil"] else 0, d["posts"]))
dry = acc["dry"]["ev"] / max(acc["dry"]["snd"], 0.1)
both = acc["flg+hall"]["ev"] / max(acc["flg+hall"]["snd"], 0.1)
print("\nflg+hall vs dry: %.3f vs %.3f = %.1fx" % (both, dry, both / max(dry, 1e-9)))
print("-> %s" % ("flanger+reverb IS worse than the harness floor"
                 if both > 1.8 * dry else
                 "NOT distinguishable from the floor at this sample size"))
