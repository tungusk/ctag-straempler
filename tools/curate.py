#!/usr/bin/env python3
"""curate — triage a sample library for the Strämpler's card.

The mechanical half of getting samples onto the card already exists in
pool_tool.py (convert to native 44.1 kHz, 8.3-safe uppercase names, batch onto a
mounted card, --dest for the per-machine folders, --bpm stamp). This is the half
that decides WHICH files are worth taking, what TYPE each is, and what it should
be CALLED.

    tools/curate.py scan  ~/Samples/Piano --out staged/piano
    tools/curate.py scan  ~/Samples --recursive --limit 400 --out staged/all
    tools/curate.py plan  staged/piano            # print the pool_tool commands

Reads only. Nothing is written outside --out, and nothing goes near the card:
`plan` prints the pool_tool invocations for you to run once you agree with the
shortlist.

WHAT IT MEASURES, and why each one matters here:

  rate/bits/ch   the module wants 44.1 kHz 16-bit; anything else needs
                 converting, which pool_tool does but which is worth knowing
                 about in bulk before it happens
  peak/rms       clipped or nearly-silent files are not worth card space
  dc             a DC offset thumps on every trigger and wastes headroom
  head silence   dead air at the start delays every hit; drums especially
  seam           |first - last| relative to the signal, for LOOPS: a loop whose
                 endpoints do not meet clicks once per cycle
  attack         time to reach 90% of peak — sharp = drum, slow = pad/keys
  pitch          YIN over the sustained portion, with a confidence. Feeds the
                 FILENAME, because Keys parses a note name out of the sample id
                 ("EP_C4", "PNOF#3") and uses it as an octave hint for auto-tune
  bpm            onset-flux autocorrelation, for loop candidates, so pool_tool
                 can stamp the deck-readable tempo and skip an analysis pass

NAMES: the card is FatFS with long names OFF, so the base must be <= 8 chars,
uppercase. That is not cosmetic — a 9-character base is an invalid 8.3 name and
f_open rejects it with EINVAL. Tonal files get the detected note in the name.

Needs sox (for decoding) and numpy. ffmpeg is not required.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import wave

import numpy as np

AUDIO_EXT = {".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg", ".m4a", ".caf", ".raw"}
ANALYSE_SECS = 12.0          # cap: enough to judge, keeps a big library tractable
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


# ---- decode ------------------------------------------------------------------
def sox_info(path):
    r = subprocess.run(["sox", "--i", path], capture_output=True, text=True)
    if r.returncode != 0:
        return None
    info = {}
    for line in r.stdout.splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            info[k.strip().lower()] = v.strip()
    return info


def decode(path, tmp="_curate_tmp.wav"):
    """-> (mono float array, samplerate) or None. Mono-summed, first ANALYSE_SECS."""
    r = subprocess.run(
        ["sox", path, "-t", "wavpcm", "-b", "16", "-e", "signed-integer",
         "-c", "1", "-r", "44100", tmp, "trim", "0", str(ANALYSE_SECS)],
        capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(tmp):
        return None
    try:
        with wave.open(tmp) as w:
            n, sr = w.getnframes(), w.getframerate()
            raw = w.readframes(n)
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
        return x, sr
    except Exception:                                  # noqa: BLE001
        return None


# ---- measurements ------------------------------------------------------------
def measure(x, sr):
    m = {}
    if len(x) == 0:
        return None
    peak = float(np.max(np.abs(x)))
    m["peak"] = peak
    m["dbfs"] = 20 * np.log10(peak) if peak > 0 else -999
    m["rms"] = float(np.sqrt(np.mean(x ** 2)))
    m["dc"] = float(np.mean(x))
    m["clipped"] = int(np.sum(np.abs(x) > 0.999))
    m["dur"] = len(x) / sr

    # head silence: first sample above 1% of peak
    thr = max(peak * 0.01, 1e-4)
    above = np.where(np.abs(x) > thr)[0]
    m["head_sil"] = float(above[0] / sr) if len(above) else m["dur"]
    m["tail_sil"] = float((len(x) - above[-1]) / sr) if len(above) else 0.0

    # attack: head to 90% of peak — sharp means percussive
    if len(above):
        i90 = np.where(np.abs(x) >= 0.9 * peak)[0]
        m["attack_ms"] = float((i90[0] - above[0]) / sr * 1000) if len(i90) else 0.0
    else:
        m["attack_ms"] = 0.0

    # loop seam: do the endpoints meet, relative to the signal's own scale
    if len(x) > 64:
        m["seam"] = float(abs(x[0] - x[-1]) / max(peak, 1e-9))
    else:
        m["seam"] = 0.0

    # brightness, for sorting by character
    n = 1 << 15
    seg = x[:n] * np.hanning(min(n, len(x))) if len(x) >= n else x * np.hanning(len(x))
    S = np.abs(np.fft.rfft(seg, n))
    f = np.fft.rfftfreq(n, 1 / sr)
    tot = float(np.sum(S))
    m["centroid"] = float(np.sum(S * f) / tot) if tot > 0 else 0.0
    return m


def yin_pitch(x, sr, fmin=40.0, fmax=1600.0):
    """-> (hz, confidence 0..1). Difference-function YIN over the sustained part.

    Same idea as util/pitch_detect on the device, kept here so triage does not
    need the module. Windowed past the attack: a transient's pitch estimate is
    meaningless and would mislabel every drum as a note.
    """
    peak = np.max(np.abs(x)) if len(x) else 0.0
    if peak < 1e-4:
        return 0.0, 0.0
    above = np.where(np.abs(x) > peak * 0.05)[0]
    if len(above) == 0:
        return 0.0, 0.0
    a = above[0] + int(0.03 * sr)                      # skip the attack
    seg = x[a:a + int(0.25 * sr)]
    if len(seg) < int(0.05 * sr):
        return 0.0, 0.0
    seg = seg - np.mean(seg)
    tmin, tmax = int(sr / fmax), int(sr / fmin)
    tmax = min(tmax, len(seg) // 2 - 1)
    if tmax <= tmin:
        return 0.0, 0.0
    # cumulative-mean-normalised difference
    d = np.empty(tmax + 1)
    for tau in range(tmin, tmax + 1):
        diff = seg[:len(seg) - tau] - seg[tau:]
        d[tau] = np.dot(diff, diff)
    d[:tmin] = d[tmin]
    cum = np.cumsum(d[tmin:]) / (np.arange(1, tmax - tmin + 2))
    nd = d[tmin:] / np.maximum(cum, 1e-12)
    tau = int(np.argmin(nd)) + tmin
    conf = float(max(0.0, 1.0 - nd[tau - tmin]))
    # parabolic refine
    if tmin < tau < tmax:
        y0, y1, y2 = d[tau - 1], d[tau], d[tau + 1]
        den = y0 - 2 * y1 + y2
        if den != 0:
            tau = tau + 0.5 * (y0 - y2) / den
    return (sr / tau if tau > 0 else 0.0), conf


def note_of(hz):
    if hz <= 0:
        return None, 0.0
    midi = 69 + 12 * np.log2(hz / 440.0)
    n = int(round(midi))
    cents = (midi - n) * 100.0
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1), float(cents)


# ---- classification ----------------------------------------------------------
def classify(m, hz, conf):
    """-> (kind, dest, reasons[]) using the module's actual folder layout."""
    reasons = []
    tonal = conf > 0.55 and hz > 0
    # PERCUSSIVE FIRST, and deliberately NOT gated on pitch. A kick is a pitched
    # sound — the first version classified on tonality and filed a 55 Hz kick
    # under usr/KEYS. Short plus a sharp attack is a one-shot whatever its pitch.
    if m["dur"] <= 2.0 and m["attack_ms"] < 25:
        if tonal:
            reasons.append("pitched one-shot (%s) — pluck or tuned drum, move it if wrong"
                           % (note_of(hz)[0] or "?"))
        return "drum", "usr/DRUMS", reasons
    # sustained and stably pitched: an instrument sample
    if tonal and m["dur"] >= 0.8:
        return "keys", "usr/KEYS", reasons
    if m["dur"] > 8.0:
        return "long", "usr", reasons
    if m["dur"] >= 1.0:
        return "loop", "usr/LOOPS", reasons
    return "other", "usr", reasons


def verdict(m, kind):
    """-> (accept, problems[]). Technical quality only — taste is not mine."""
    p = []
    if m["dbfs"] < -40:
        p.append("very quiet (%.0f dBFS)" % m["dbfs"])
    if m["clipped"] > 32:
        p.append("clipped (%d samples at full scale)" % m["clipped"])
    if abs(m["dc"]) > 0.02:
        p.append("DC offset %.3f" % m["dc"])
    if kind == "drum" and m["head_sil"] > 0.02:
        p.append("%.0f ms dead air before the hit" % (m["head_sil"] * 1000))
    if kind == "loop" and m["seam"] > 0.25:
        p.append("loop seam mismatch %.2f — will click once per cycle" % m["seam"])
    return (len(p) == 0), p


def short_name(path, kind, note, used):
    """8.3-safe uppercase base, <= 8 chars. A 9-char base is an INVALID 8.3 name
    and f_open rejects it with EINVAL — this is a hard firmware constraint."""
    stem = os.path.splitext(os.path.basename(path))[0]
    # Drop tokens that carry no identity: velocity layers, take numbers, and a
    # note name already in the source name (we append the DETECTED one, and
    # "piano_A3_soft" + "A3" produced PIANOAA3 in the first version).
    stem = re.sub(r"(?i)\b(vel(ocity)?|v|take|tk|rr|layer|samp(le)?)\s*\d+\b", "", stem)
    stem = re.sub(r"(?i)([A-G])(#|b)?(-?\d)(?![0-9])", "", stem)
    stem = re.sub(r"[^A-Za-z0-9]", "", stem).upper()
    if kind == "keys" and note:
        n = re.sub(r"[^A-G#0-9]", "", note.upper())
        base = (stem[:max(1, 8 - len(n))] + n)[:8]
    else:
        pre = {"drum": "DR", "loop": "LP", "long": "TR"}.get(kind, "")
        base = (pre + stem[:8 - len(pre)])[:8] if pre else stem[:8]
    base = base or "SMP"
    cand, i = base, 1
    while cand in used:
        tail = str(i)
        cand = base[:8 - len(tail)] + tail
        i += 1
    used.add(cand)
    return cand


# ---- commands ----------------------------------------------------------------
def cmd_scan(args):
    files = []
    for root, _dirs, names in os.walk(args.folder):
        for nm in sorted(names):
            if os.path.splitext(nm)[1].lower() in AUDIO_EXT and not nm.startswith("."):
                files.append(os.path.join(root, nm))
        if not args.recursive:
            break
    if args.limit:
        files = files[:args.limit]
    if not files:
        raise SystemExit("no audio files found in %s" % args.folder)
    os.makedirs(args.out, exist_ok=True)
    print("%d files under %s\n" % (len(files), args.folder))

    used, rows = set(), []
    for i, path in enumerate(files):
        dec = decode(path)
        if not dec:
            rows.append({"src": path, "kind": "UNREADABLE", "accept": False,
                         "problems": ["sox could not decode it"]})
            continue
        x, sr = dec
        m = measure(x, sr)
        if m is None:
            continue
        hz, conf = yin_pitch(x, sr)
        note, cents = note_of(hz) if conf > 0.55 else (None, 0.0)
        kind, dest, reasons = classify(m, hz, conf)
        accept, problems = verdict(m, kind)
        info = sox_info(path) or {}
        rows.append({
            "src": path, "kind": kind, "dest": dest,
            "name": short_name(path, kind, note, used),
            "accept": accept, "problems": problems + reasons,
            "note": note, "cents": round(cents, 1), "pitch_conf": round(conf, 2),
            "dur": round(m["dur"], 2), "dbfs": round(m["dbfs"], 1),
            "attack_ms": round(m["attack_ms"], 1), "seam": round(m["seam"], 3),
            "dc": round(m["dc"], 4), "clipped": m["clipped"],
            "centroid": int(m["centroid"]),
            "srcrate": info.get("sample rate", "?"),
            "srcbits": info.get("precision", "?"),
            "srcch": info.get("channels", "?"),
        })
        if (i + 1) % 25 == 0:
            print("  ...%d/%d" % (i + 1, len(files)), flush=True)

    man = os.path.join(args.out, "manifest.json")
    with open(man, "w") as fh:
        json.dump(rows, fh, indent=2)

    ok = [r for r in rows if r.get("accept")]
    print("\n%-26s %-6s %-9s %-7s %-7s %-6s %s"
          % ("source", "kind", "name", "dur", "note", "dBFS", "problems"))
    for r in rows:
        print("%-26s %-6s %-9s %-7s %-7s %-6s %s"
              % (os.path.basename(r["src"])[:26], r.get("kind", "?"),
                 r.get("name", "-"), r.get("dur", "-"),
                 (r.get("note") or "-"), r.get("dbfs", "-"),
                 "; ".join(r.get("problems", []))[:60]))
    print("\n%d of %d pass the technical checks" % (len(ok), len(rows)))
    by = {}
    for r in ok:
        by[r["dest"]] = by.get(r["dest"], 0) + 1
    for d, n in sorted(by.items()):
        print("   %-12s %d" % (d, n))
    print("\nmanifest: %s" % man)
    print("taste is yours — edit \"accept\" in the manifest, then: curate.py plan %s"
          % args.out)


def cmd_plan(args):
    rows = json.load(open(os.path.join(args.out, "manifest.json")))
    ok = [r for r in rows if r.get("accept")]
    if not ok:
        raise SystemExit("nothing accepted in the manifest")
    print("# review, then run. pool_tool converts to the module's native format,")
    print("# writes the .JSN sidecar and makes the name 8.3-safe.")
    print("# --card must point at the MOUNTED card, e.g. /Volumes/STRAMPLER")
    by = {}
    for r in ok:
        by.setdefault(r["dest"], []).append(r)
    for dest, group in sorted(by.items()):
        print("\n# %s  (%d files)" % (dest, len(group)))
        for r in group:
            print("tools/pool_tool.py convert %s --card \"$CARD\" --dest %s"
                  % (repr(r["src"]), dest))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("folder")
    s.add_argument("--out", default="staged")
    s.add_argument("--recursive", action="store_true")
    s.add_argument("--limit", type=int, default=0)
    s.set_defaults(fn=cmd_scan)
    p = sub.add_parser("plan")
    p.add_argument("out")
    p.set_defaults(fn=cmd_plan)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
