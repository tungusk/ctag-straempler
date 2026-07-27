#!/usr/bin/env python3
"""Find the burst: sample steps a band-limited signal cannot physically produce.

Tuned against a LABELED example (2026-07-26, burst_hunt.wav): Arlo played for
20 s with the artefact audible, and four mid-note events showed up with steps of
0.476 / 0.265 / 0.109 / 0.037 while the music itself never exceeded 0.029.

Why this threshold and not a hand-picked one: a signal band-limited to f_max at
amplitude A cannot slew faster than 2*pi*f_max*A per sample period. Arlo's patch
is low-passed at 872 Hz, which puts the ceiling at ~0.029 — exactly where dozens
of "events" from a naive threshold were piling up, because they WERE the music.
Anything above that ceiling did not come from the signal.

Rather than trusting a nominal cutoff, the ceiling is measured per capture from a
high percentile of the signal's own slew. Bursts are rare, so they do not move a
99.9th percentile; the music defines its own ceiling and the outliers stand out.

MID/SIDE matters. The labeled event kept the mono sum perfectly continuous while
the channel DIFFERENCE flipped sign in a single sample. A note attack does the
opposite — it is mid-dominant and spread over many samples. Reporting both lets
a real burst be told from a hard-played note without listening.
"""
import numpy as np

from rig import load_wav


def slew_ceiling(d, k=1.6, pct=99.9):
    """The largest step the material itself produces, times a margin."""
    base = float(np.percentile(d[d > 0], pct)) if np.any(d > 0) else 0.0
    return max(k * base, 1e-4), base


def find_bursts(x, sr, k=1.6, gap_ms=50.0):
    """-> list of events. x is [frames, channels]."""
    if x.ndim == 1:
        x = x[:, None]
    if x.shape[1] >= 2:
        mid, side = x[:, 0] + x[:, 1], x[:, 0] - x[:, 1]
    else:
        mid, side = x[:, 0] * 2.0, np.zeros(len(x))
    dm, ds = np.abs(np.diff(mid)), np.abs(np.diff(side))
    d = np.maximum(dm, ds)
    thr, base = slew_ceiling(d, k)

    hits = np.where(d > thr)[0]
    gap = int(gap_ms / 1000.0 * sr)
    events, run = [], []
    for i in hits:
        if run and i - run[-1] < gap:
            run.append(i)
        else:
            if run:
                events.append(run)
            run = [i]
    if run:
        events.append(run)

    out = []
    for e in events:
        i0, i1 = e[0], e[-1]
        peak = int(e[int(np.argmax(d[e]))])
        # Judge mid vs side over the WHOLE event, not the single largest sample.
        # At the labeled 29.114 s event the strongly side-dominant step is the
        # sample NEXT TO the biggest one (side 0.248 / mid 0.024), while the
        # biggest sample itself is mixed — so a single-sample test called it
        # "mid" and threw away the signature.
        ev_ds, ev_dm = float(np.max(ds[e])), float(np.max(dm[e]))
        out.append({
            "t": i0 / sr,
            "dur_ms": (i1 - i0) / sr * 1000.0,
            "n": len(e),
            "max_step": float(np.max(d[e])),
            "over_ceiling": float(np.max(d[e]) / base) if base else 0.0,
            "max_mid": ev_dm,
            "max_side": ev_ds,
            # side-dominant => channel-difference discontinuity (the labeled
            # signature). mid-dominant => more like a hard attack.
            "side_dominant": bool(ev_ds > 2.0 * ev_dm),
            "peak_sample": peak,
        })
    return out, thr, base


def report(path, k=1.6):
    x, sr = load_wav(path)
    ev, thr, base = find_bursts(x, sr, k)
    print("%s  %.1f s  material slew ceiling %.4f  threshold %.4f"
          % (path, len(x) / sr, base, thr))
    if not ev:
        print("   no events")
        return ev
    print("   %-9s %-9s %-7s %-8s %-8s %-8s %s"
          % ("t (s)", "step", "x ceil", "dur ms", "mid", "side", "shape"))
    for e in ev:
        print("   %-9.3f %-9.4f %-7.1f %-8.1f %-8.4f %-8.4f %s"
              % (e["t"], e["max_step"], e["over_ceiling"], e["dur_ms"],
                 e["max_mid"], e["max_side"],
                 "SIDE — channel-difference step" if e["side_dominant"] else "mid"))
    return ev


if __name__ == "__main__":
    import sys
    for p in (sys.argv[1:] or ["burst_hunt.wav"]):
        report(p)
        print()
