#!/usr/bin/env python3
"""Does MOVING the reverb mix pop? The gd/gw no-slew hypothesis, isolated.

  tools/bench/mix_sweep.py [seconds_per_condition]

The reverb's equal-power gains are recomputed per block from rv->wet with no
slew (reverb.c), so anything that moves the mix steps the dry/wet ratio once
per 1.45 ms block. The CV matrix can move it continuously, which would make a
train of small broadband steps — a candidate for the still-open "occasional
pop from reverb" now that the NaN-flush lead has weakened (counter reads zero).

Method: hold a note with hall reverb on and capture the analog output under
three conditions that differ ONLY in what the same-cadence parameter posts say:

  static  posts rvmx=40 every 250 ms (same value — controls for the HTTP load,
          which glitched audio on its own before the DMA deepen, and for
          whatever autosave activity a /remote/params post causes)
  fine    rvmx walks 34..46 in steps of 3 — a slow CV sweep
  step    rvmx alternates 15 <-> 65 — a square-ish LFO into the matrix

If the no-slew hypothesis is right, events scale with step size: step > fine >
static. If all three read alike, remote mix motion does not pop and the search
moves elsewhere. `sav` is read once per capture (after it stops) to confirm the
autosave load really was comparable across arms.

Deliberately NOT loading a reference tone: this runs against whatever sample is
loaded so it hears the patch Arlo actually plays. Loop seams are part of the
floor, but the floor is shared by all three arms and reps are interleaved.
"""
import json
import sys
import time

import numpy as np

from rig import Rig, load_wav, dbfs, post, CAPTURE_OPEN_S
from detect import find_bursts

NOTE = 57
DEFAULT_S = 20.0
POST_GAP_S = 0.25          # 4 Hz — fast enough to look like a CV sweep per block
# cut/res/mxa are in this list because of the 2026-07-28 lesson: a patch can
# arrive with the filter closed (cut 221 Hz, res 0.52) and Cutoff <- CV5 at
# full amount in the matrix, and a note held into that is SILENT at the jack.
# assert-silent caught exactly that on this script's first run (2026-07-30).
TOUCHED = ["fxsl", "rv", "rvmx", "atk", "sus", "rel", "cut", "res", "mxa"]

CONDITIONS = [
    ("static", lambda i: 40),
    ("fine",   lambda i: 34 + 3 * (i % 5)),          # 34 37 40 43 46
    ("step",   lambda i: 15 if i % 2 else 65),
]


def measure(rig, name, mixval, seconds, rep):
    label = "mix_%s_r%d" % (name, rep)
    post("/remote/params", json.dumps({"rvmx": 40}))
    time.sleep(1.5)                                  # let the tank refill at base
    sav0 = (rig.status().get("sav") or {}).get("n", 0) if isinstance(
        rig.status().get("sav"), dict) else rig.status().get("sav")
    proc = rig.capture(seconds, "%s.wav" % label)
    time.sleep(CAPTURE_OPEN_S)
    t0 = time.time()
    rig.note_on(NOTE)
    last_note = t0
    i = 0
    while time.time() - t0 < seconds - 0.5:
        post("/remote/params", json.dumps({"rvmx": mixval(i)}))
        i += 1
        if time.time() - last_note > 1.4:            # MIDI liveness heartbeat
            rig.note_on(NOTE)
            last_note = time.time()
        time.sleep(POST_GAP_S)
    proc.wait(timeout=seconds + 25)
    rig.notes_off()
    st = rig.status()                                # ONE read, after the capture
    sav1 = (st.get("sav") or {}).get("n", 0) if isinstance(st.get("sav"), dict) else st.get("sav")

    x, sr = load_wav("%s.wav" % label)
    peak = float(np.max(np.abs(x)))
    if peak < 0.01:
        raise SystemExit("take %s is silent (peak %.4f) — is the module patched in?"
                         % (label, peak))
    ev, thr, base = find_bursts(x, sr)
    dur = len(x) / sr
    return {"name": name, "rep": rep, "events": len(ev), "rate": len(ev) / dur,
            "dur": dur, "posts": i, "sav": (sav0, sav1),
            "biggest": max([e["max_step"] for e in ev], default=0.0),
            "ceiling": base, "peak_dbfs": dbfs(x[:, 0])}


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_S
    rig = Rig()
    rig.check_device()
    if not rig.calib:
        raise SystemExit("no calib.json — run calib.py first")
    st = rig.status()
    if st.get("recording"):
        raise SystemExit("ABORT: the module is RECORDING")
    if st.get("machine") != "Keys":
        raise SystemExit("switch to Keys first (machine=%s)" % st.get("machine"))

    before = rig.params()
    saved = {k: before[k] for k in TOUCHED if k in before}
    print("saved for restore: %s" % saved)

    rig.notes_off()
    rig.set_params({"fxsl": [0, 0], "rv": 2, "rvmx": 40,
                    "atk": 0.005, "sus": 1.0, "rel": 0.4,
                    "cut": 18000.0, "res": 0.2, "mxa": [0] * 8}, verify=False)
    time.sleep(2.5)

    rows = []
    try:
        # interleaved reps: a drift in conditions cannot masquerade as an effect
        for rep in range(2):
            for name, fn in CONDITIONS:
                r = measure(rig, name, fn, seconds, rep)
                rows.append(r)
                print("  rep%d  %-6s : %2d events in %.1f s = %.3f/s   biggest %.4f "
                      "vs ceiling %.4f   %d posts  sav %s->%s   %.1f dBFS"
                      % (rep, name, r["events"], r["dur"], r["rate"], r["biggest"],
                         r["ceiling"], r["posts"], r["sav"][0], r["sav"][1],
                         r["peak_dbfs"]))
    finally:
        if saved:
            rig.set_params(saved, verify=False)
            print("restored: %s" % saved)

    print("\n--- verdict ---")
    base = None
    for name, _ in CONDITIONS:
        rr = [r["rate"] for r in rows if r["name"] == name]
        if not rr:
            continue
        m = sum(rr) / len(rr)
        if name == "static":
            base = m
        tag = ""
        if base is not None and name != "static":
            tag = ("  (baseline zero — events here are attributable)"
                   if base < 1e-6 else "  = %.2fx static" % (m / base))
        print("   %-6s : %.3f events/s  %s%s" % (name, m, [round(v, 3) for v in rr], tag))
    print("\n   If step >> fine >> static, the un-slewed gd/gw is real — the fix")
    print("   is a one-pole slew on the gains in reverb.c. If all alike, remote")
    print("   mix motion does not pop and the hunt moves elsewhere.")


if __name__ == "__main__":
    main()
