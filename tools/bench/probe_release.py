#!/usr/bin/env python3
"""Is the event tied to the note's RELEASE, or to something else?

The events cluster at +0.19..0.56 s after a note onset (median +0.40), and the
envelope reaches zero around +0.50 s with a 200 ms gate and a 300 ms release. So
the release is the obvious suspect — but "it lands where the release is" is
correlation. Moving the release and watching the event follow is causation.

Sweeps release time and gate length, several notes each, and reports where the
discontinuities land relative to the note onset. If the events track the release,
the answer is in the envelope/voice path. If they stay at +0.4 s regardless, it
is something else with its own clock and the release is a coincidence.
"""
import json
import time

import numpy as np

from detect import find_bursts
from rig import Rig, load_wav
from soak import note_onsets

rig = Rig()
base = rig.params()
NOTES = 6


def trial(tag, gate_ms, rel, extra=None):
    p = dict(base)
    p["rel"] = rel
    if extra:
        p.update(extra)
    rig.set_params(p, verify=False)
    time.sleep(0.8)
    wav = "probe_%s.wav" % tag
    secs = NOTES * 3.0 + 2.0
    proc = rig.capture(secs, wav)
    time.sleep(0.6)
    t0 = time.time()
    trig = []
    for _ in range(NOTES):
        trig.append(time.time() - t0)
        rig.trigger(gate_ms)
        time.sleep(3.0)
    proc.wait(timeout=secs + 25)

    x, sr = load_wav(wav)
    ons = note_onsets(x, sr)
    ev, thr, ceil = find_bursts(x, sr)
    rel_times = []
    for e in ev:
        if not ons:
            break
        near = min(ons, key=lambda o: abs(e["t"] - o))
        dt = e["t"] - near
        if -0.1 <= dt <= 2.5:
            rel_times.append((dt, e["max_step"], e["dur_ms"]))
    rel_times.sort()
    # drop the attack itself (within 60 ms of the onset)
    body = [r for r in rel_times if r[0] > 0.06]
    print("%-22s gate %4d ms  rel %.2f s  -> %d onsets, %d events past the attack"
          % (tag, gate_ms, rel, len(ons), len(body)))
    if body:
        d = np.array([b[0] for b in body])
        print("       t-onset: %s" % " ".join("%.3f" % v for v in d[:10]))
        print("       median %.3f s   steps %s"
              % (np.median(d), " ".join("%.2f" % b[1] for b in body[:8])))
    else:
        print("       (none)")
    return [b[0] for b in body]


try:
    print("baseline patch: rel %.2f  atk %.4f  dec %.2f  sus %.2f"
          % (base.get("rel", 0), base.get("atk", 0), base.get("dec", 0), base.get("sus", 0)))
    print()
    trial("rel0.30_gate200", 200, 0.30)
    trial("rel1.20_gate200", 200, 1.20)
    trial("rel0.05_gate200", 200, 0.05)
    trial("rel0.30_gate900", 900, 0.30)
finally:
    rig.set_params(base, verify=False)
    print("\npatch restored")
