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

TARGET_DBFS = -6.0
TOLERANCE_DB = 8.0          # generous: we care about "sane", not "exact"
CLIP_DBFS = -0.5


def measure(rig, seconds, path, play):
    """Capture `seconds`, optionally holding a note, -> per-channel stats."""
    proc = rig.capture(seconds, path)
    time.sleep(0.4)                       # let sox open the device
    if play:
        rig.hold_gate(seconds - 1.2)
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

    print("\n--- 2. signal (holding a note, 5 s) ---")
    sig, x, sr = measure(rig, 5.0, "cal_signal.wav", play=True)
    for c, s in enumerate(sig):
        print("   ch%d  peak %.5f (%6.1f dBFS)  rms %.6f" % (c + 1, s["peak"], s["dbfs"], s["rms"]))

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
        if spread > TOLERANCE_DB:
            problems.append("channels differ by %.1f dB — match the gains" % spread)
    hottest = max(s["dbfs"] for s in sig)
    if hottest < TARGET_DBFS - TOLERANCE_DB:
        problems.append("signal is quiet (%.1f dBFS, want about %.0f) — turn the gain up"
                        % (hottest, TARGET_DBFS))

    snr = hottest - dbfs(np.array([noise]))
    print("   live channels : %s" % [c + 1 for c in live])
    print("   hottest peak  : %.1f dBFS  (target %.0f)" % (hottest, TARGET_DBFS))
    print("   noise floor   : %.1f dBFS" % dbfs(np.array([noise])))
    print("   signal/noise  : %.1f dB" % snr)

    if problems:
        print("\n   NOT READY:")
        for p in problems:
            print("     - %s" % p)
        raise SystemExit(1)
    print("\n   rig OK")

    if verify_only:
        if rig.calib:
            old = rig.calib["signal_dbfs"]
            drift = [abs(a - b) for a, b in zip(old, [s["dbfs"] for s in sig])]
            print("   drift vs stored calibration: %s dB" % [round(d, 1) for d in drift])
            if max(drift) > 3.0:
                raise SystemExit("gain has MOVED since calibration — recapture baselines")
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
        "noise_dbfs": [s["dbfs"] for s in floor],
        # the floor every detector threshold is derived from, rather than a
        # number picked out of the air
        "noise_peak": noise,
        "patch": rig.params(),
    })
    print("   wrote calib.json — do not touch the interface gain from here on")


if __name__ == "__main__":
    main()
