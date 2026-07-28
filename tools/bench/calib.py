#!/usr/bin/env python3
"""Check the bench rig and record the interface calibration.

Run this once after patching, and again any time the Scarlett's gain knobs move.
Everything downstream asserts against calib.json, so a nudged gain knob shows up
as a failed check rather than as a mysterious level difference between sessions.

  tools/bench/calib.py            # full check + write calib.json
  tools/bench/calib.py --verify   # check the stored calibration still holds

Checks, in order:
  1. both channels carry signal          (a mono patch analysed as stereo is silent nonsense)
  2. headroom                            (peak should sit near -6 dBFS, never clipping)
  3. channel balance                     (L and R within a few dB of each other)
  4. noise floor with nothing playing    (sets every later detector's threshold)
"""
import json
import sys
import time

import numpy as np

from rig import Rig, load_wav, dbfs, CAPTURE_OPEN_S

# What we are setting the gain FOR is the module's FULL SCALE arriving with a
# little headroom — NOT whatever note happens to be playing arriving at -6. The
# first version of this file conflated the two, and since the reference patch ran
# at lvl 0.30 with FX it demanded a gain that put module full scale about 8 dB
# ABOVE 0 dBFS. Captures of anything loud would have clipped silently, which no
# later analysis can detect or undo.
#
# So the criterion is on the EXTRAPOLATED full-scale arrival: the level the
# interface would see if the module output a full-amplitude signal. The module's
# own peak meter (`vu`, 0..255, post-limiter) gives the factor to divide out.
TARGET_FS_DBFS = -6.0       # where module full scale should land
FS_TOLERANCE_DB = 4.0       # accept -10..-2
CLIP_DBFS = -0.5
MIN_SNR_DB = 40.0

# The reference the gain is set against: a resident test tone, dry, level 1.0, so
# a later session can reproduce the exact signal rather than trusting a number.
#
# cut/res are wide open deliberately, so the reference is the SAMPLE and not the
# filter. Both live on physical knobs 6 and 7 under Keys' takeover rule: a remote
# write holds until the knob physically MOVES, and is overridden every block while
# it is moving. On 2026-07-28 an LFO patched to CV6 was sweeping cutoff, so this
# patch's cut=18000 came back as 316 Hz and the whole calibration ran through a
# filter in motion — unmeasurable, and indistinguishable from a level fault.
# check_static() refuses to calibrate in that state; the set_params readback
# catches a knob merely parked somewhere else.
REF_PATCH = {"smp": "TSTF3", "lvl": 1.0, "base": 57, "atk": 0.005, "dec": 0.01,
             "sus": 1.0, "rel": 0.05, "cut": 18000.0, "e2c": 0.0,
             "fxsl": [0, 0], "rv": 0, "lm": 1, "lx": 600}
# res is deliberately absent: knob 7 sits wherever Arlo left it (0.57 on
# 2026-07-28) and resonance at an 18 kHz cutoff does nothing to a 220 Hz tone, so
# demanding a value here only produced a permanent false alarm on the readback.
# Its actual position is recorded in calib.json's knob_params.
REF_NOTE = 57
MOVE_TOL = {"cut": 20.0, "res": 0.02}      # absolute wander allowed over ~2 s


def check_static(rig, seconds=2.5):
    """Refuse to calibrate while a knob or CV is MOVING.

    A reference has to be reproducible. Anything modulating cutoff or resonance —
    a patched LFO, a hand on a knob — changes the tone's amplitude during the very
    capture that defines the gain mapping, and leaves no trace in the result
    except a number that will not reproduce.
    """
    seen = {}
    t0 = time.time()
    while time.time() - t0 < seconds:
        p = rig.params()
        for k in MOVE_TOL:
            v = p.get(k)
            if isinstance(v, (int, float)):
                seen.setdefault(k, []).append(float(v))
        time.sleep(0.35)
    moving = {}
    for k, vals in seen.items():
        span = max(vals) - min(vals)
        if span > MOVE_TOL[k]:
            moving[k] = (span, min(vals), max(vals))
    return moving, {k: (sum(v) / len(v)) for k, v in seen.items()}


def module_peak_dbfs(rig, samples=6):
    """The module's OWN output peak, dBFS relative to its full scale.

    Read with the note still held but AFTER the capture has closed, never during
    it: polling /status at 4 Hz through a capture pushed `auspk` from ~1300 to
    1495-1517 us of a 1450 us budget and was AUDIBLE (2026-07-26). The reference
    tone is a steady loop, so a reading taken a second later is the same reading.
    """
    pk = 0
    for _ in range(samples):
        vu = rig.status().get("vu") or [0, 0, 0, 0]
        pk = max(pk, vu[2], vu[3])
        time.sleep(0.25)
    return (20.0 * np.log10(pk / 255.0)) if pk else None


def measure(rig, seconds, path, play):
    """Capture `seconds`, optionally holding a note, -> per-channel stats."""
    proc = rig.capture(seconds, path)
    time.sleep(CAPTURE_OPEN_S)            # let sox actually open the device
    if play:
        rig.hold_note(seconds - 1.2, REF_NOTE)
    proc.wait(timeout=seconds + 20)
    x, sr = load_wav(path)
    return [{"peak": float(np.max(np.abs(x[:, c]))),
             "dbfs": dbfs(x[:, c]),
             "rms": float(np.sqrt(np.mean(x[:, c] ** 2)))}
            for c in range(x.shape[1])], x, sr


def main():
    verify_only = "--verify" in sys.argv
    rig = Rig()
    rig.check_device()

    st = rig.status()
    print("module: %s   recording=%s" % (st.get("machine"), st.get("recording")))
    if st.get("recording"):
        raise SystemExit("ABORT: the module is RECORDING — do not disturb it")

    print("\n--- 1. noise floor (nothing playing, 3 s) ---")
    floor, _, _ = measure(rig, 3.0, "cal_floor.wav", play=False)
    for c, s in enumerate(floor):
        print("   ch%d  peak %.5f (%6.1f dBFS)  rms %.6f" % (c + 1, s["peak"], s["dbfs"], s["rms"]))
    noise = max(s["peak"] for s in floor)

    print("\n--- 2. signal (reference tone, holding a note, 5 s) ---")
    rig.set_params(REF_PATCH)
    back = rig.params()
    fr = int(back.get("le") or 0)
    if fr > 8000:      # loop INSIDE the tone: looping the whole file re-attacks
        rig.set_params({"lm": 1, "ls": int(fr * 0.3), "le": int(fr * 0.7), "lx": 600})
    print("   reference: %s lvl %.2f note %d" % (back.get("smp"), back.get("lvl") or 0, REF_NOTE))
    moving, level = check_static(rig)
    print("   knob-owned params: %s" % {k: round(v, 1) for k, v in level.items()})
    if moving:
        for k, (span, lo, hi) in moving.items():
            print("   %s is MOVING: %.1f .. %.1f (span %.1f)" % (k, lo, hi, span))
        raise SystemExit("ABORT: something is modulating %s — unpatch the CV (or take "
                         "your hand off the knob) and re-run. Calibrating against a "
                         "moving filter produces a mapping that cannot reproduce."
                         % "/".join(moving))
    sig, x, sr = measure(rig, 5.0, "cal_signal.wav", play=True)
    for c, s in enumerate(sig):
        print("   ch%d  peak %.5f (%6.1f dBFS)  rms %.6f" % (c + 1, s["peak"], s["dbfs"], s["rms"]))

    # module-side peak, so the arriving level can be extrapolated to full scale
    rig.note_on(REF_NOTE)
    mod_db = module_peak_dbfs(rig)
    rig.notes_off()
    print("   module output peak: %s"
          % ("%.1f dBFS of its own full scale" % mod_db if mod_db is not None
             else "NOT MEASURABLE (module vu read 0 — is it playing?)"))

    problems = []
    live = [c for c, s in enumerate(sig) if s["peak"] > max(noise * 4, 0.005)]
    print("\n--- verdict ---")
    if len(live) < 2:
        problems.append("only channel(s) %s carry signal — patch BOTH module "
                        "outputs (L->input 1, R->input 2)" % [c + 1 for c in live])
    for c, s in enumerate(sig):
        if s["dbfs"] > CLIP_DBFS:
            problems.append("ch%d is CLIPPING (%.1f dBFS) — turn the gain down" % (c + 1, s["dbfs"]))
    if len(live) == 2:
        spread = abs(sig[0]["dbfs"] - sig[1]["dbfs"])
        if spread > 6.0:
            problems.append("channels differ by %.1f dB — match the gains" % spread)
    hottest = max(s["dbfs"] for s in sig)

    # the actual criterion: where module FULL SCALE would land
    fs_db = None
    if mod_db is not None:
        fs_db = hottest - mod_db
        if fs_db > TARGET_FS_DBFS + FS_TOLERANCE_DB:
            problems.append("module full scale would arrive at %.1f dBFS — too hot, "
                            "no headroom. Turn the gain DOWN about %.0f dB"
                            % (fs_db, fs_db - TARGET_FS_DBFS))
        elif fs_db < TARGET_FS_DBFS - FS_TOLERANCE_DB:
            problems.append("module full scale would arrive at %.1f dBFS — quiet, "
                            "wasting resolution. Turn the gain UP about %.0f dB"
                            % (fs_db, TARGET_FS_DBFS - fs_db))

    snr = hottest - dbfs(np.array([noise]))
    if snr < MIN_SNR_DB:
        problems.append("only %.0f dB above the noise floor — check the patch cables" % snr)
    print("   live channels : %s" % [c + 1 for c in live])
    print("   reference tone: %.1f dBFS at the interface" % hottest)
    if fs_db is not None:
        print("   MODULE FULL SCALE would arrive at %.1f dBFS  (target %.0f +/- %.0f)"
              % (fs_db, TARGET_FS_DBFS, FS_TOLERANCE_DB))
    print("   noise floor   : %.1f dBFS" % dbfs(np.array([noise])))
    print("   signal/noise  : %.1f dB" % snr)

    if problems:
        print("\n   NOT READY:")
        for p in problems:
            print("     - %s" % p)
        raise SystemExit(1)
    print("\n   rig OK")

    if verify_only:
        if not rig.calib:
            raise SystemExit("no calib.json — run without --verify first")
        # Compare the FULL-SCALE figure, not the raw arriving level: the raw level
        # also moves when a knob filters the reference tone, which is not a gain
        # fault and must not be reported as one.
        old_fs = rig.calib.get("fullscale_dbfs")
        if old_fs is not None and fs_db is not None:
            print("   full-scale arrival: %.1f dBFS now vs %.1f stored (drift %.1f dB)"
                  % (fs_db, old_fs, abs(fs_db - old_fs)))
            if abs(fs_db - old_fs) > 3.0:
                raise SystemExit("INTERFACE GAIN HAS MOVED since calibration — "
                                 "earlier takes are not comparable; re-run calib.py "
                                 "and recapture any baseline you care about")
        for k, was in (rig.calib.get("knob_params") or {}).items():
            now = level.get(k)
            if now is not None and abs(now - was) > max(MOVE_TOL[k] * 4, abs(was) * 0.25):
                print("   note: %s is %.1f, was %.1f at calibration — a knob moved. "
                      "Not a gain fault, but the tone is not the same tone." % (k, now, was))
        print("   calibration still holds")
        return

    print("\n--- 3. wall-clock -> recording-time offset ---")
    # Needed so an unattended run can tell ITS OWN NOTES from the artefact. The
    # offset between wall clock and recording time is a real, measurable quantity
    # (sox device-open + HTTP round trip + the module's own latency); guessing it
    # would be fitting the classification to the desired answer.
    lat = []
    for _ in range(3):
        proc = rig.capture(3.0, "cal_lat.wav")
        time.sleep(CAPTURE_OPEN_S)         # let sox actually open the device
        t_go = time.time()
        rig.trigger(200)
        proc.wait(timeout=25)
        x, sr = load_wav("cal_lat.wav")
        # sox began recording ~0.6 s before t_go by construction; find the onset
        m = np.abs(x).mean(axis=1)
        k = int(0.005 * sr)
        env = np.sqrt(np.mean((m[:len(m) // k * k].reshape(-1, k)) ** 2, axis=1))
        loud = np.where(env > 0.25 * env.max())[0]
        if len(loud) == 0:
            continue
        onset = loud[0] * 0.005
        if onset < 0.05:      # residual tail from the previous note, not our onset
            continue
        lat.append(onset)
        time.sleep(0.8)
    if lat:
        trig_lat = float(np.median(lat))
        print("   samples %s -> a trigger at seg_t0 lands at %.3f s in the recording"
              % ([round(v, 3) for v in lat], trig_lat))
    else:
        trig_lat = 0.0
        print("   could not measure — leaving at 0")

    rig.save_calib({
        "trigger_offset": trig_lat,
        "device": rig.device,
        "rate": rig.rate,
        # what the interface reads for a known module output, so later sessions
        # can tell "the module got quieter" from "someone moved the gain knob"
        "signal_dbfs": [s["dbfs"] for s in sig],
        # the extrapolated full-scale arrival is the number the gain was actually
        # set against; signal_dbfs alone cannot distinguish "gain moved" from
        # "the reference patch changed"
        "fullscale_dbfs": fs_db,
        "module_peak_dbfs": mod_db,
        "ref_patch": REF_PATCH,
        "ref_note": REF_NOTE,
        # knob-owned, unsettable, and part of what the reference tone sounded
        # like — stored so --verify can say "a knob moved" instead of "the level
        # is different and nobody knows why"
        "knob_params": level,
        "noise_dbfs": [s["dbfs"] for s in floor],
        # the floor every detector threshold is derived from, rather than a
        # number picked out of the air
        "noise_peak": noise,
        "patch": rig.params(),
    })
    print("   wrote calib.json — do not touch the interface gain from here on")


if __name__ == "__main__":
    main()
