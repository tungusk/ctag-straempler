#!/usr/bin/env python3
"""Does ESP-NOW traffic cost audio? The measurement that gates ad-hoc sync.

  tools/bench/espnow_cost.py [seconds_per_condition]

The plan for units talking to each other directly rests on ESP-NOW. Before any of
it gets built, one question decides whether the approach is viable on this
hardware: does transmitting at sync rates disturb the audio path?

It is a fair worry rather than a theoretical one. Ordinary HTTP requests to this
module underran the I2S DMA on ~28% of requests until the buffer was deepened
(drivers/i2s_per.c) — same radio, same CPU. If ESP-NOW costs audio at 25-100 Hz,
the direction needs rethinking now, not after it is built.

Method: hold a note, capture the analog output, count discontinuities with the
detector already validated against a labelled burst capture (detect.py), and
compare rates across ESP-NOW rates. The note is held over MIDI, not /remote/trig,
and /status is NOT polled during a capture — both of those are known to perturb
what is being measured.

One unit is enough: what is being measured is the local cost of TRANSMITTING,
which needs no peer to receive.
"""
import sys
import time

import numpy as np

from rig import Rig, load_wav, dbfs, post, get, CAPTURE_OPEN_S
from detect import find_bursts

RATES = [0, 25, 100]      # Hz. 25 ~ a beat-clock tick rate, 100 = deliberately hard
NOTE = 57
DEFAULT_S = 20.0

PATCH = {"smp": "TSTC2", "lm": 1, "lvl": 0.8, "base": NOTE, "quant": True,
         "atk": 0.005, "dec": 0.01, "sus": 1.0, "rel": 0.05,
         "cut": 18000.0, "e2c": 0.0, "fxsl": [0, 0], "rv": 0}
# THE LOOP SEAM IS PART OF THE FLOOR unless you kill it. Looping the whole 2 s
# tone wraps mid-cycle, which is a real discontinuity every 2 s — the first run of
# this script measured 0.275 events/s at REST and that was almost entirely seams,
# a floor high enough to hide a small effect. A window inside the tone with a long
# crossfade removes it, so what is left is attributable.
LOOP = {"lm": 1, "ls": 26460, "le": 61740, "lx": 1200}


def measure(rig, hz, seconds, label):
    post("/espnow?hz=%d" % hz)
    time.sleep(1.0)
    proc = rig.capture(seconds, "%s.wav" % label)
    time.sleep(CAPTURE_OPEN_S)
    # hold over MIDI, refreshed under the 5 s liveness timeout. No /status polling
    # here: at 4 Hz it pushed auspk to 1495-1517 us of a 1450 budget and was
    # AUDIBLE (2026-07-26), which would land in the very measurement being taken.
    t0 = time.time()
    rig.note_on(NOTE)
    while time.time() - t0 < seconds - 1.0:
        time.sleep(1.4)
        rig.note_on(NOTE)
    proc.wait(timeout=seconds + 25)
    rig.notes_off()

    x, sr = load_wav("%s.wav" % label)
    peak = float(np.max(np.abs(x)))
    if peak < 0.01:
        raise SystemExit("take %s is silent (peak %.4f) — is the module patched in?" % (label, peak))
    ev, thr, base = find_bursts(x, sr)
    dur = len(x) / sr
    return {"hz": hz, "events": len(ev), "rate": len(ev) / dur, "dur": dur,
            "peak_dbfs": dbfs(x[:, 0]), "biggest": max([e["max_step"] for e in ev], default=0.0),
            "ceiling": base}


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

    rig.notes_off()
    rig.set_params(PATCH, verify=False)
    time.sleep(2.5)
    rig.set_params(LOOP, verify=False)
    time.sleep(1.5)
    b = rig.params()
    print("signal: %s loop %s..%s xfade %s" % (b.get("smp"), b.get("ls"), b.get("le"), b.get("lx")))

    rows = []
    # each rate twice, interleaved, so a drift in conditions cannot masquerade as
    # an effect — a number that does not repeat is not a measurement
    for rep in range(2):
        for hz in RATES:
            r = measure(rig, hz, seconds, "espnow_%dhz_r%d" % (hz, rep))
            r["rep"] = rep
            rows.append(r)
            print("  rep%d  %3d Hz : %2d events in %.1f s = %.3f/s   biggest %.4f vs ceiling %.4f   %.1f dBFS"
                  % (rep, hz, r["events"], r["dur"], r["rate"], r["biggest"], r["ceiling"], r["peak_dbfs"]))
    post("/espnow?hz=0")

    print("\n--- verdict ---")
    base = None
    for hz in RATES:
        rr = [r["rate"] for r in rows if r["hz"] == hz]
        m = sum(rr) / len(rr)
        if hz == 0:
            base = m
        tag = ""
        if base is not None and hz != 0:
            if base < 1e-6:
                tag = "  (baseline is zero — any events here are attributable)"
            else:
                tag = "  = %.2fx baseline" % (m / base)
        print("   %3d Hz : %.3f events/s  %s%s" % (hz, m, [round(v, 3) for v in rr], tag))
    print("\n   %s" % get("/espnow").decode())


if __name__ == "__main__":
    main()
