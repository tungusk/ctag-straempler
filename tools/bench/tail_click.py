#!/usr/bin/env python3
"""Measure the click when a Keys one-shot runs off the end of its sample.

  tools/bench/tail_click.py [label]

Plays a NON-LOOPING sample and holds the note past the end of the audio, then
measures the largest sample-to-sample step in the moment the voice stops against
the step the material itself produces while playing. A click is a discontinuity,
so the honest measure is slew, not level: the end of a sample is quiet, and a
level meter reads a click at the end of a decay as nothing at all.

The reference tones are ideal material for this. A 2.000 s tone at 261.63 Hz is
523.26 cycles, so it ends a quarter-cycle from zero — the worst case, and an
exactly reproducible one. Zeroing the voice there steps the output by most of
the waveform's amplitude in a single sample.
"""
import sys
import time

import numpy as np

from rig import Rig, load_wav, dbfs, CAPTURE_OPEN_S

SAMPLE = "TSTC2"
NOTE = 60          # root, so it plays at native rate and ends after exactly 2 s
CAP_S = 5.0


def slew(x):
    return np.abs(np.diff(x))


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "tail"
    rig = Rig()
    rig.check_device()
    if not rig.calib:
        raise SystemExit("no calib.json — run calib.py first")
    if rig.status().get("recording"):
        raise SystemExit("ABORT: the module is RECORDING")

    # one-shot, filter open, dry, and a long release so the ENVELOPE cannot be
    # what silences the note — we want the sample END, not a release ramp
    rig.set_params({"smp": SAMPLE, "lm": 0, "lvl": 1.0, "base": NOTE,
                    "quant": True, "gld": 0.0,
                    "atk": 0.005, "dec": 0.01, "sus": 1.0, "rel": 2.0,
                    "cut": 18000.0, "e2c": 0.0, "fxsl": [0, 0], "rv": 0},
                   verify=False)
    time.sleep(3.0)
    back = rig.params()
    print("sample %s  frames %s  lm %s  (holding note %d past the end)"
          % (back.get("smp"), back.get("le"), back.get("lm"), NOTE))

    proc = rig.capture(CAP_S, "%s.wav" % label)
    time.sleep(CAPTURE_OPEN_S)
    rig.note_on(NOTE)          # held: the gate never releases during the capture
    proc.wait(timeout=CAP_S + 25)
    rig.notes_off()

    x, sr = load_wav("%s.wav" % label)
    ch = x[:, int(np.argmax([dbfs(x[:, c]) for c in range(x.shape[1])]))]
    env = np.abs(ch)
    k = int(0.002 * sr)
    sm = np.convolve(env, np.ones(k) / k, mode="same")
    loud = np.where(sm > 0.15 * sm.max())[0]
    if len(loud) < sr // 10:
        raise SystemExit("no note found — is the module patched in?")
    a, b = int(loud[0]), int(loud[-1])
    print("note runs %.3f .. %.3f s (%.3f s)" % (a / sr, b / sr, (b - a) / sr))

    # STEADY reference: slew while the sample is clearly playing, away from both
    # the attack and the end. This is the ceiling a click has to beat to be real.
    body = ch[a + int(0.20 * sr):b - int(0.20 * sr)]
    if len(body) < sr // 5:
        raise SystemExit("note too short to measure")
    ceil = float(np.max(slew(body)))
    p999 = float(np.percentile(slew(body), 99.9))

    # THE END: the last 30 ms of audible signal plus 20 ms after it
    e0, e1 = max(a, b - int(0.030 * sr)), min(len(ch), b + int(0.020 * sr))
    tail = ch[e0:e1]
    tmax = float(np.max(slew(tail)))
    ti = int(np.argmax(slew(tail)))

    print("\n  steady slew ceiling : %.6f   (99.9th pct %.6f)" % (ceil, p999))
    print("  worst step at end   : %.6f  at %.4f s (%.1f ms before the end)"
          % (tmax, (e0 + ti) / sr, (b - (e0 + ti)) * 1000.0 / sr))
    print("  ratio               : %.2fx the steady ceiling" % (tmax / ceil if ceil else 0))
    print("  verdict             : %s"
          % ("CLICK — the end steps harder than the material ever does"
             if tmax > ceil * 1.5 else
             "clean — the end is within the material's own slew"))


if __name__ == "__main__":
    main()
