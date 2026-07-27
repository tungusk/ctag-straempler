#!/usr/bin/env python3
"""Does the event's phase follow the GATE LENGTH?

The events are rare (~3-4% of notes) but tightly phase-locked when they happen,
and the one seen with a 900 ms gate landed at exactly +0.900 s — the moment the
gate closes. If that is the mechanism, lengthening the gate must move the whole
cluster with it. If the cluster stays put, the gate is a coincidence.

Needs hundreds of notes per condition to see a cluster at a 3-4% hit rate, so
this runs long. Conditions are INTERLEAVED in short blocks rather than run
back-to-back, so slow drift (temperature, wifi, whatever) cannot masquerade as a
difference between them.

  tools/bench/probe_long.py [minutes_per_condition]
"""
import json
import sys
import time

import numpy as np

from detect import find_bursts
from rig import Rig, load_wav
from soak import note_onsets

MINUTES = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
NOTE_EVERY = 2.5
SEG = 30.0
CONDITIONS = [("gate200", 200), ("gate900", 900)]

rig = Rig()
base = rig.params()
print("patch: smp %r fxsl %s rv %s  rel %.2f" %
      (base.get("smp"), base.get("fxsl"), base.get("rv"), base.get("rel", 0)))
print("%.0f min per condition, interleaved in %.0f s blocks\n" % (MINUTES, SEG))

results = {name: [] for name, _ in CONDITIONS}
notes = {name: 0 for name, _ in CONDITIONS}
t_end = time.time() + MINUTES * 60 * len(CONDITIONS)
blk = 0
try:
    while time.time() < t_end:
        name, gate_ms = CONDITIONS[blk % len(CONDITIONS)]
        blk += 1
        wav = "pl_%s.wav" % name
        proc = rig.capture(SEG, wav)
        time.sleep(0.6)
        t0 = time.time()
        n = 0
        while time.time() - t0 < SEG - 2.0:
            rig.trigger(gate_ms)
            n += 1
            time.sleep(NOTE_EVERY)
        try:
            proc.wait(timeout=SEG + 20)
        except Exception:                          # noqa: BLE001
            proc.kill()
            continue
        notes[name] += n
        x, sr = load_wav(wav)
        ons = note_onsets(x, sr)
        ev, thr, ceil = find_bursts(x, sr)
        for e in ev:
            if not ons:
                break
            near = min(ons, key=lambda o: abs(e["t"] - o))
            dt = e["t"] - near
            if 0.06 < dt <= 2.5:                   # past the attack, inside the note
                results[name].append((dt, e["max_step"], e["dur_ms"]))
        done = sum(notes.values())
        print("  block %d (%s): %d notes so far, hits g200=%d g900=%d"
              % (blk, name, done, len(results["gate200"]), len(results["gate900"])), flush=True)
finally:
    rig.set_params(base, verify=False)

print("\n=== RESULT ===")
for name, gate_ms in CONDITIONS:
    r = results[name]
    print("%-8s gate %3d ms   %d notes, %d events (%.1f%%)"
          % (name, gate_ms, notes[name], len(r), 100.0 * len(r) / max(notes[name], 1)))
    if r:
        d = np.array([v[0] for v in r])
        print("         t-onset  median %.3f  mean %.3f  min %.3f  max %.3f"
              % (np.median(d), d.mean(), d.min(), d.max()))
        print("         all: %s" % " ".join("%.3f" % v for v in np.sort(d)[:20]))
json.dump({k: v for k, v in results.items()}, open("probe_long.json", "w"), indent=2)
print("\nwrote probe_long.json")
