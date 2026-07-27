#!/usr/bin/env python3
"""Unattended hunt for the intermittent burst.

  tools/bench/soak.py --minutes 90
  tools/bench/soak.py --hours 3 --no-drive     # Arlo plays; we only listen

Records in back-to-back segments, runs detect.py over each, and keeps a clip plus
a /status snapshot for every hit. Segments with nothing in them are deleted, so a
three-hour run costs disk only for the interesting moments.

WHY IT IS SHAPED THIS WAY

- /status is read ONCE per segment, AFTER the recording stops — never during.
  Polling at 4 Hz while a note sustained pushed peak block cost to 1495-1517 us
  of a 1450 us budget and Arlo HEARD it. Every device meter we care about
  (ausgap, auspk, sav) is peak-hold and cleared on read, so one read after a
  segment covers the whole segment anyway. Polling more often would only add
  interference and tell us nothing extra.
- Note triggering is deliberately sparse. The gate endpoint caps at 2000 ms, so
  SUSTAINING a note needs a POST every ~1.5 s — continuous wifi traffic next to
  an audio path whose ADC is documented as wifi-spike-sensitive. Instead we
  trigger and let the note ring out, one POST every few seconds.
- There is a small gap between segments while sox restarts. It is reported
  honestly as a duty cycle rather than pretended away: a rare event might land in
  a gap, which just means the run needs to be longer.
"""
import argparse
import json
import os
import shutil
import time

import numpy as np

from detect import find_bursts
from rig import Rig, load_wav, CAPTURE_OPEN_S


def note_onsets(x, sr, hop=0.005, rise=6.0):
    """When did notes actually SOUND, from the envelope alone.

    Deliberately independent of the discontinuity detector. The previous version
    fitted the offset by maximising alignment between triggers and DETECTED
    EVENTS, then measured how far events sat from the nearest trigger — which is
    circular, and duly produced a beautiful cluster at +0.4 s that was partly an
    artifact of the fit. Onsets are big, unambiguous, and have nothing to do with
    the events we are trying to classify.
    """
    k = max(1, int(hop * sr))
    m = np.abs(x).mean(axis=1) if x.ndim > 1 else np.abs(x)
    env = np.sqrt(np.mean((m[:len(m) // k * k].reshape(-1, k)) ** 2, axis=1)) + 1e-12
    floor = np.percentile(env, 20)
    out, last = [], -1e9
    for i in range(2, len(env)):
        t = i * hop
        if env[i] > max(rise * floor, 0.01) and env[i] > 3.0 * env[i - 2] and t - last > 0.4:
            out.append(t)
            last = t
    return out


def fit_offset(onsets, triggers, lo=-0.5, hi=2.0, step=0.005, tol=0.12):
    """Solve for the ONE unknown — where the recording starts relative to our
    trigger timestamps — using ONSETS, not detected events.

    A stored constant cannot work: sox's device-open time jitters by a couple of
    hundred milliseconds run to run (measured 0.52 and 0.73 on consecutive
    calibrations), which is larger than the matching tolerance.

    Vectorised. The scalar version did offsets x events x triggers comparisons in
    pure Python — about 12 million per segment — and was most of why a 3 hour run
    only recorded 40 minutes of audio.
    """
    if not onsets or not triggers:
        return 0.0, 0
    on = np.asarray(onsets)[:, None]
    tr = np.asarray(triggers)[None, :]
    offs = np.arange(lo, hi, step)
    # |onset - (trigger + off)| <= tol, counted per offset
    diff = on - tr                                   # [n_onsets, n_triggers]
    counts = np.array([np.any(np.abs(diff - o) <= tol, axis=1).sum() for o in offs])
    i = int(np.argmax(counts))
    return float(offs[i]), int(counts[i])


def explain(t, triggers, sample_secs, attack_tol=0.12, end_tol=0.12):
    """Is this event just the note we played?

    ATTACK  — right after a trigger; a note onset is legitimately a fast transient.
    END     — at trigger + sample length. Keys zeroes the voice once the read
              cursor passes the last frame, with no fade, so a sample that does
              not end at zero steps straight to silence. Real, audible, and NOT
              the artefact we are hunting — it fires on every note.
    ?       — unexplained, which is the whole point of the run.
    """
    for tr in triggers:
        # symmetric: the fitted offset is an estimate, so an event landing just
        # BEFORE the predicted onset is still that onset. An asymmetric window
        # made the fit and the classifier disagree — the fit said 6/6 accounted
        # while the classifier still reported four of them as hits.
        if abs(t - tr) <= attack_tol:
            return "attack"
        if sample_secs and abs((t - tr) - sample_secs) <= end_tol:
            return "sample-end"
    return "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=0.0)
    ap.add_argument("--minutes", type=float, default=0.0)
    ap.add_argument("--seg", type=float, default=30.0, help="segment length, seconds")
    ap.add_argument("--outdir", default="soak")
    ap.add_argument("--no-drive", action="store_true", help="do not trigger notes")
    ap.add_argument("--note-every", type=float, default=4.0)
    ap.add_argument("-k", type=float, default=1.6, help="detector margin over the material ceiling")
    args = ap.parse_args()

    total = args.hours * 3600 + args.minutes * 60
    if total <= 0:
        total = 1800.0
    os.makedirs(args.outdir, exist_ok=True)
    log_path = os.path.join(args.outdir, "soak.log")

    rig = Rig()
    rig.check_device()
    st = rig.status()
    if st.get("recording"):
        raise SystemExit("ABORT: the module is RECORDING")
    patch = rig.params()
    print("machine %s   sample %r   fxsl %s   rv %s"
          % (st.get("machine"), patch.get("smp"), patch.get("fxsl"), patch.get("rv")))
    print("soaking for %.0f min in %.0f s segments -> %s/" % (total / 60, args.seg, args.outdir))

    def log(msg):
        line = "%s  %s" % (time.strftime("%H:%M:%S"), msg)
        print(line, flush=True)
        with open(log_path, "a") as fh:
            fh.write(line + "\n")

    # how long a note lasts before the voice runs off the end of the sample —
    # keys_load_zone sets loop_end to the whole file, so this doubles as the
    # length for a non-looping sample
    sample_secs = (patch.get("le") or 0) / 44100.0
    log("START  patch smp=%r fxsl=%s rv=%s  sample %.2f s  loop=%s"
        % (patch.get("smp"), patch.get("fxsl"), patch.get("rv"), sample_secs,
           "on" if patch.get("lm") else "off"))
    # measured in calib.py; without it the run cannot tell its own notes apart
    trig_off = (rig.calib or {}).get("trigger_offset", 0.0)
    log("trigger offset %.3f s (from calib.json; recording time of a trigger at seg_t0)"
        % trig_off)
    if not trig_off:
        log("WARNING: no trigger offset — run calib.py or every note reads as a hit")
    t_end = time.time() + total
    seg_i, hits_total, recorded, accounted = 0, 0, 0.0, 0
    t0 = time.time()

    while time.time() < t_end:
        seg_i += 1
        raw = os.path.join(args.outdir, "seg_%04d.wav" % seg_i)
        proc = rig.capture(args.seg, raw)
        time.sleep(CAPTURE_OPEN_S)
        seg_t0 = time.time()
        # Remember WHEN we triggered, relative to the segment. Without this the
        # detector reports every note we played ourselves: an attack is a real
        # transient, and a sample running out mid-gate stops dead (Keys zeroes
        # the voice once pos passes the end — no fade), which is a genuine step
        # at trigger + sample length. A soak that cannot tell those from the
        # artefact just collects clips of itself.
        triggers = []
        # HARD wall-clock bound. This loop used to be `while proc.poll() is None`
        # with nothing else: when sox wedged (it did, ~1 h into a 3 h run, still
        # "recording" a 30 s file an hour later) the loop span forever and the
        # whole soak silently stopped making progress.
        deadline = seg_t0 + args.seg + 10.0
        if not args.no_drive:
            next_note = time.time()
            while proc.poll() is None and time.time() < deadline:
                if time.time() >= next_note:
                    try:
                        # timestamp BEFORE the request: the note starts when the
                        # device sees it, not when the HTTP round trip returns
                        triggers.append(time.time() - seg_t0)
                        rig.trigger(200)
                    except Exception as exc:          # noqa: BLE001
                        log("trigger failed: %s" % exc)
                    next_note = time.time() + args.note_every
                time.sleep(0.2)
        try:
            proc.wait(timeout=max(2.0, deadline - time.time()))
        except Exception:                             # noqa: BLE001 - TimeoutExpired
            log("seg %d: sox WEDGED — killing and continuing" % seg_i)
            proc.kill()
            try:
                proc.wait(timeout=5)
            except Exception:                         # noqa: BLE001
                pass
            if os.path.exists(raw):
                os.remove(raw)
            continue
        recorded += args.seg

        # ONE status read, after the fact — peak-holds cover the whole segment
        try:
            snap = rig.status(fx=False)
        except Exception as exc:                      # noqa: BLE001
            snap = {"error": str(exc)}

        try:
            x, sr = load_wav(raw)
        except Exception as exc:                      # noqa: BLE001
            log("seg %d unreadable: %s" % (seg_i, exc))
            continue
        events, thr, base = find_bursts(x, sr, k=args.k)
        # Offset from ONSETS — independent of the events being classified.
        onsets = note_onsets(x, sr)
        fitted, matched = fit_offset(onsets, triggers)
        for e in events:
            e["cause"] = explain(e["t"], [t + fitted for t in triggers], sample_secs,
                                 attack_tol=0.14, end_tol=0.14)
        unexplained = [e for e in events if e["cause"] == "?"]
        peak = float(abs(x).max())
        if peak < 0.005:
            log("seg %d SILENT (peak %.5f) — is the module still playing?" % (seg_i, peak))
            os.remove(raw)
            continue

        if unexplained:
            hits_total += len(unexplained)
            keep = os.path.join(args.outdir, "hit_%04d.wav" % seg_i)
            shutil.move(raw, keep)
            with open(os.path.join(args.outdir, "hit_%04d.json" % seg_i), "w") as fh:
                json.dump({"segment": seg_i, "file": os.path.basename(keep),
                           "threshold": thr, "material_ceiling": base,
                           "fitted_offset": fitted, "matched": matched,
                           "onsets": onsets,
                           "triggers": triggers, "sample_secs": sample_secs,
                           "events": events, "status_after": snap}, fh, indent=2)
            log("seg %d offset %.3f s from %d onsets (%d matched), %d/%d events accounted for"
                % (seg_i, fitted, len(onsets), matched, len(events) - len(unexplained), len(events)))
            for e in unexplained:
                log("HIT seg %d  t=%.3f in-seg  step %.4f (%.1fx ceiling)  %s  | ausgap %s auspk %s sav.n %s"
                    % (seg_i, e["t"], e["max_step"], e["over_ceiling"],
                       "SIDE" if e["side_dominant"] else "mid",
                       snap.get("ausgap"), snap.get("auspk"), (snap.get("sav") or {}).get("n")))
        else:
            os.remove(raw)
            if seg_i % 10 == 0:
                log("seg %d clean (%d unexplained hits so far, %d accounted for, %.0f min recorded)"
                    % (seg_i, hits_total, accounted, recorded / 60))
        accounted += len(events) - len(unexplained)

    elapsed = time.time() - t0
    log("DONE  %d segments, %.0f min recorded of %.0f min elapsed (duty %.0f%%), "
        "%d UNEXPLAINED hits, %d accounted for as attack/sample-end"
        % (seg_i, recorded / 60, elapsed / 60, 100 * recorded / elapsed, hits_total, accounted))


if __name__ == "__main__":
    main()
