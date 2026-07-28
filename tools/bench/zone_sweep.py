#!/usr/bin/env python3
"""Play a multisample across its zone boundaries and MEASURE what comes out.

  tools/bench/zone_sweep.py            # build the test set, sweep, report
  tools/bench/zone_sweep.py --keep     # sweep whatever zones are already loaded

Answers three things that cannot be checked from the module's own reports:

  1. Is the right zone chosen? `vz` says which one the engine picked, but a wrong
     pick is INAUDIBLE as pitch — varispeed corrects any zone to the right note,
     which is why three earlier attempts to infer zone selection from pitch or
     duration all failed. Only the TIMBRE differs, so the check has to be
     "correct zone" against "pitch still right", separately.
  2. Is the pitch right in CENTS across a boundary? Each zone carries its own
     root+fine from auto-tune. If those disagree, a scale walks in tune inside a
     zone and jumps at the seam — the classic multisample fault, and one nobody
     hears as a bug, only as "this instrument feels wrong".
  3. Does the level jump at the seam? Different source samples, different peaks.

The reference tones make this measurable: their true pitch is known by
construction, so cent error is against an absolute, not against the instrument's
own average. FLATA4 is deliberately ~13 cents flat, so it also proves per-zone
`fine` is actually applied and not quietly dropped.
"""
import json
import subprocess
import sys
import time

import numpy as np

from rig import Rig, load_wav, dbfs, fundamental, CAPTURE_OPEN_S

# roots are NOT asserted here — auto-tune decides them on load, and what it
# decides is part of what we are measuring
ZONE_SET = ["TSTC2", "TSTF3", "TUNEA3", "FLATA4"]

BASE_PATCH = {"lvl": 1.0, "quant": True, "gld": 0.0,
              "atk": 0.005, "dec": 0.01, "sus": 1.0, "rel": 0.05,
              "cut": 18000.0, "e2c": 0.0,      # filter open: it colours f0 detection
              "fxsl": [0, 0], "rv": 0,         # dry: reverb smears the pitch estimate
              "atl": True}                     # auto-tune each zone as it loads

CAP_S = 2.0
SETTLE_S = 0.5


def cents(f_meas, f_want):
    return 1200.0 * np.log2(f_meas / f_want) if f_meas > 0 and f_want > 0 else float("nan")


def midi_hz(n):
    return 440.0 * (2.0 ** ((n - 69) / 12.0))


def build_zones(rig):
    """Load the test set as zones, deliberately sending a CONFLICTING flat key.

    preset_load used to apply the legacy flat lm/ls/le/lx to zone 0 AFTER the
    zones array, so a partial post carrying both silently lost zone 0's loop and
    tuning. Sending lm=0 alongside zones asking for lm=1 makes that regression
    fail this script instead of going unnoticed.
    """
    patch = dict(BASE_PATCH)
    patch["zones"] = [{"smp": s, "lm": 1, "lx": 600} for s in ZONE_SET]
    patch["lm"] = 0                      # the conflicting legacy key
    rig.set_params(patch, verify=False)
    time.sleep(2.0 + 1.2 * len(ZONE_SET))    # each zone is an SD read
    return rig.params()


def capture_note(rig, note, path):
    proc = rig.capture(CAP_S + 1.0, path)
    time.sleep(CAPTURE_OPEN_S)
    rig.note_on(note)
    time.sleep(CAP_S)
    zone = rig.params().get("vz")         # read while the note is still held
    rig.notes_off()
    proc.wait(timeout=CAP_S + 25)
    x, sr = load_wav(path)
    # window on the NOTE, not the file: the head is pre-onset silence and the
    # tail is release, and including either drags the estimate around
    m = np.abs(x).mean(axis=1)
    k = max(1, int(0.005 * sr))
    env = np.sqrt(np.mean(m[:len(m) // k * k].reshape(-1, k) ** 2, axis=1))
    loud = np.where(env > 0.3 * env.max())[0]
    if len(loud) < 20:
        return None
    a, b = int(loud[0] * k), int(loud[-1] * k)
    a += int(0.15 * sr)                  # skip the attack transient
    b -= int(0.05 * sr)
    if b - a < int(0.2 * sr):
        return None
    seg = x[a:b]
    return {"zone": zone,
            "f0": fundamental(seg[:, 0], sr),
            "dbfs": max(dbfs(seg[:, 0]), dbfs(seg[:, 1])),
            "frames": b - a}


def main():
    rig = Rig()
    rig.check_device()
    if not rig.calib:
        raise SystemExit("no calib.json — run calib.py first")
    st = rig.status()
    if st.get("recording"):
        raise SystemExit("ABORT: the module is RECORDING")
    if st.get("machine") != "Keys":
        raise SystemExit("switch the module to Keys first (machine=%s)" % st.get("machine"))
    rig.notes_off()

    if "--keep" in sys.argv:
        back = rig.params()
        rig.set_params({k: v for k, v in BASE_PATCH.items() if k != "atl"}, verify=False)
        time.sleep(1.5)
        back = rig.params()
    else:
        print("loading %d zones: %s" % (len(ZONE_SET), ", ".join(ZONE_SET)))
        back = build_zones(rig)

    nz = back.get("nz")
    zones = back.get("zones") or []
    print("nz=%s  arena_used=%s frames (%.2f MB)"
          % (nz, back.get("au"), (back.get("au") or 0) * 2 / 1e6))
    if not zones:
        raise SystemExit("module reported no zones — load failed (load_err=%r)" % back.get("lerr"))
    # TUNE_* from instsampler_priv.h. CONFLICT/NAME are not failures in
    # themselves, but they say the root rests on the FILENAME rather than on
    # anything heard — which is precisely how TSTC2 ended up two octaves out.
    TSRC = {0: "none", 1: "audio", 2: "audio+name-octave", 3: "NAME ONLY", 4: "CONFLICT"}
    roots = []
    for i, z in enumerate(zones):
        r, fn = z.get("root"), z.get("fn") or 0.0
        roots.append(r + fn)
        print("   zone %d  %-8s root %3s  fine %+6.1f cents  tune=%-17s lm %s  loop %s..%s"
              % (i, z.get("smp"), r, fn * 100.0, TSRC.get(z.get("ts"), z.get("ts")),
                 z.get("lm"), z.get("ls"), z.get("le")))

    # UNREACHABLE ZONES. keys_zone_for_note takes the nearest root and, on a tie,
    # the FIRST — so two zones sharing a root make the later one dead: it occupies
    # arena and never sounds. Auto-tune produces this readily (TSTF3 and TUNEA3
    # both contain A3, so both landed on root 57), and nothing in the machine says
    # so; the zone is simply silent forever.
    for i, ri in enumerate(roots):
        earlier = [j for j, rj in enumerate(roots[:i]) if abs(rj - ri) < 0.5]
        if earlier:
            print("   note: zone %d (%s) is UNREACHABLE — root %.2f duplicates zone %d"
                  % (i, zones[i].get("smp"), ri, earlier[0]))

    # THE REGRESSION CHECK: zone 0 asked for lm=1 and a stale flat lm=0 rode along
    if not ("--keep" in sys.argv):
        if (zones[0].get("lm") or 0) == 0:
            print("\n   *** zone 0 lost its loop to the legacy flat key — "
                  "the preset_load ordering bug is BACK ***")

    # notes bracketing every nearest-root boundary, plus one inside each zone
    bounds = [(roots[i] + roots[i + 1]) / 2.0 for i in range(len(roots) - 1)]
    notes = set()
    for r in roots:
        notes.add(int(round(r)))
    for b in bounds:
        notes.update([int(np.floor(b)) - 1, int(np.floor(b)), int(np.ceil(b)) + 1])
    notes = sorted(n for n in notes if 24 <= n <= 96)
    print("\nsweeping %d notes: %s" % (len(notes), notes))
    print("\n note  want Hz   got Hz   cents   zone  level     expected zone")
    rows = []
    for n in notes:
        r = capture_note(rig, n, "zsweep_%d.wav" % n)
        time.sleep(SETTLE_S)
        if not r:
            print("  %3d   SILENT / too short — no usable window" % n)
            rows.append({"note": n, "ok": False})
            continue
        want = midi_hz(n)
        c = cents(r["f0"], want)
        # nearest root, the same rule keys_zone_for_note uses
        exp = int(np.argmin([abs(n - rr) for rr in roots]))
        flag = "" if r["zone"] == exp else "  <-- MISMATCH"
        print("  %3d  %8.2f %8.2f  %+7.1f   %3s   %6.1f dBFS  %d%s"
              % (n, want, r["f0"], c, r["zone"], r["dbfs"], exp, flag))
        rows.append({"note": n, "ok": True, "cents": c, "zone": r["zone"],
                     "expected": exp, "dbfs": r["dbfs"]})

    good = [r for r in rows if r.get("ok")]
    if not good:
        raise SystemExit("nothing measurable — is the module patched to the interface?")
    print("\n--- verdict ---")
    bad_zone = [r for r in good if r["zone"] != r["expected"]]
    print("   zone selection : %d/%d correct%s"
          % (len(good) - len(bad_zone), len(good),
             "" if not bad_zone else "   WRONG at notes %s" % [r["note"] for r in bad_zone]))
    cs = np.array([r["cents"] for r in good])
    print("   pitch error    : mean %+.1f cents, worst %+.1f, spread %.1f"
          % (cs.mean(), cs[np.argmax(np.abs(cs))], cs.max() - cs.min()))
    # a seam fault shows as a JUMP between adjacent notes in different zones,
    # not as a large absolute error — the whole instrument can be 20 cents flat
    # and still feel right, while a 15-cent step at one seam does not
    worst_seam = None
    for a, b in zip(good, good[1:]):
        if a["zone"] != b["zone"]:
            step = abs(b["cents"] - a["cents"])
            if worst_seam is None or step > worst_seam[0]:
                worst_seam = (step, a["note"], b["note"], a["zone"], b["zone"])
    if worst_seam:
        step, n1, n2, z1, z2 = worst_seam
        print("   worst seam     : %.1f cents between note %d (zone %d) and %d (zone %d)"
              % (step, n1, z1, n2, z2))
        print("                    %s" % ("OK — under the ~10 cent audibility floor"
                                          if step < 10 else "AUDIBLE — zones disagree on tuning"))
    lv = np.array([r["dbfs"] for r in good])
    print("   level spread   : %.1f dB across the keyboard" % (lv.max() - lv.min()))


if __name__ == "__main__":
    main()
