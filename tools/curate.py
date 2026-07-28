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


def parse_name_note(path, middle_c_octave=3):
    """Pull a note out of the FILENAME, e.g. 'Patch-D#-1-127.wav' -> 'D#-1'.

    Libraries disagree about octave numbering: this Moog set calls middle C "C3"
    (Yamaha), scientific notation calls it C4. Measured to confirm — its "C3"
    file is 254.75 Hz, i.e. middle C. `middle_c_octave` converts to scientific
    so a named note and a detected one can actually be compared.

    Treated as a HINT, never as truth — the same rule the device uses in
    pitch_name_hint: the recording beats the label. Here it also catches the
    case where detection fails outright (very low notes), and flags real
    disagreements instead of silently choosing.
    """
    stem = os.path.splitext(os.path.basename(path))[0]
    # TOKENISE rather than regex the whole stem. Some folders name files
    # "Instrument 1-C0-32-F8GB.wav" — name, note, velocity, then a random hash —
    # and a loose search reads "F8GB" as the note F8 and picks the hash as the
    # velocity. Prefer a note token that is FOLLOWED by a numeric one, which is
    # the velocity, and that pins both fields at once.
    # Match the note AND the velocity as a PAIR. Splitting on "-" tears a
    # NEGATIVE octave apart — "Basic Pulse 226-A-1-127.wav" tokenises to
    # [...,'A','1','127'] and the note "A-1" disappears, which silently dropped
    # every bottom-octave file in three folders. Requiring "<sep>NOTE<sep>DIGITS"
    # also rejects the trailing hash in "Instrument 1-C0-32-F8GB.wav".
    pair = re.findall(r"[-_\s]([A-G](?:#|b)?-?\d)[-_\s](\d+)", stem)
    if pair:
        mm = re.match(r"^([A-G])(#|b)?(-?\d)$", pair[-1][0])
    else:
        bare = re.findall(r"[-_\s]([A-G](?:#|b)?-?\d)(?![0-9])", stem)
        mm = re.match(r"^([A-G])(#|b)?(-?\d)$", bare[-1]) if bare else None
    if mm is None:
        return None, 0.0
    letter, acc, octv = mm.groups()
    semi = NOTE_NAMES.index(letter)
    if acc == "#":
        semi += 1
    elif acc == "b":
        semi -= 1
    octv = int(octv) + (4 - middle_c_octave)      # -> scientific
    midi = (octv + 1) * 12 + semi
    if not (0 <= midi <= 127):
        return None, 0.0
    hz = 440.0 * 2 ** ((midi - 69) / 12.0)
    return "%s%d" % (NOTE_NAMES[midi % 12], midi // 12 - 1), hz


def parse_name_vel(path):
    """Velocity = the numeric token right after the note token. See above: the
    last number in the filename can be a hash, not a velocity."""
    stem = os.path.splitext(os.path.basename(path))[0]
    pair = re.findall(r"[-_\s]([A-G](?:#|b)?-?\d)[-_\s](\d+)", stem)
    if pair:
        return int(pair[-1][1])
    nums = [int(t) for t in re.split(r"[-_\s]+", stem) if t.isdigit()]
    return max(nums) if nums else 0


def yin_pitch(x, sr, fmin=22.0, fmax=1600.0):
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
    # WINDOW LENGTH SCALES WITH fmin. A fixed 0.25 s window is only ~8 cycles at
    # 32 Hz and the estimate collapses — measured confidence 0.41 at 60 Hz and
    # 0.01 at 40 Hz on real Moog bass notes, which then fell through the
    # classifier as "not pitched". Ask for ~12 periods of the lowest note we
    # claim to detect, and give the difference function room for two lags.
    want = max(0.25, 12.0 / fmin)
    a = above[0] + int(0.03 * sr)                      # skip the attack
    seg = x[a:a + int(want * sr)]
    if len(seg) < int(4.0 / fmin * sr):
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


def resolve_pitch(det_hz, conf, named_hz):
    """Combine a measured pitch with the filename's claim.

    Mirrors the device's rule in pitch_name_hint: the NAME may fix the OCTAVE
    when the two agree on pitch class, and the recording decides everything else.
    Needed because YIN makes octave errors on deep bass — measured on this Moog
    set, a 65 Hz note detected an octave low (-1228 cents) and a 33 Hz one an
    octave high, while everything above ~130 Hz was solid at conf >= 0.8.

    -> (hz, source) where source is 'audio', 'audio+name octave', or 'name'.
    """
    if named_hz <= 0:
        return (det_hz, "audio") if conf > 0.55 else (0.0, "none")
    if det_hz <= 0 or conf < 0.35:
        return named_hz, "name"
    # fold the ratio to within half an octave: same pitch class?
    ratio = det_hz / named_hz
    octaves = round(np.log2(ratio))
    folded = ratio / (2.0 ** octaves)
    cents = 1200.0 * np.log2(folded)
    if abs(cents) <= 65.0:
        # agrees on pitch class. Keep the measured DEVIATION, take the named
        # octave — that is the combination that survives an octave error.
        if octaves != 0:
            return named_hz * folded, "audio+name octave"
        return det_hz, "audio"
    return (det_hz, "audio") if conf > 0.75 else (named_hz, "name")


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
        named, named_hz = parse_name_note(path, args.middle_c)
        rhz, psrc = resolve_pitch(hz, conf, named_hz)
        # NAME FROM THE FILENAME when the library provides one, and use the
        # measurement to VALIDATE it — not the other way round. A properly
        # multisampled set already encodes intent, and renaming "C2" to "B2"
        # because the instrument is 58 cents flat would fight both the library
        # and Keys' own note-name hint. The deviation is metadata, not a rename.
        off = float(1200.0 * np.log2(rhz / named_hz)) if (rhz > 0 and named_hz > 0) else 0.0
        note = named or (note_of(rhz)[0] if rhz > 0 else None)
        cents = off
        kind, dest, reasons = classify(m, rhz, conf if psrc.startswith("audio") else 0.6)
        if named and rhz > 0 and abs(off) > 120.0:
            reasons.append("LABEL SUSPECT: named %s but measures %s (%+.0f cents)"
                           % (named, note_of(rhz)[0], off))
        accept, problems = verdict(m, kind)
        info = sox_info(path) or {}
        rows.append({
            "src": path, "kind": kind, "dest": dest,
            "name": short_name(path, kind, note, used),
            "accept": accept, "problems": problems + reasons,
            "note": note, "cents": round(cents, 1), "pitch_conf": round(conf, 2),
            "pitch_src": psrc,
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
    devs = [r["cents"] for r in rows if r.get("note") and r.get("pitch_src", "").startswith("audio")
            and abs(r["cents"]) < 120]
    print("\n%-26s %-6s %-9s %-7s %-7s %-6s %s"
          % ("source", "kind", "name", "dur", "note", "dBFS", "problems"))
    for r in rows:
        print("%-26s %-6s %-9s %-7s %-7s %-6s %s"
              % (os.path.basename(r["src"])[:26], r.get("kind", "?"),
                 r.get("name", "-"), r.get("dur", "-"),
                 (r.get("note") or "-"), r.get("dbfs", "-"),
                 "; ".join(r.get("problems", []))[:60]))
    if devs:
        med = float(np.median(devs))
        print("\nTUNING: median %+.0f cents vs A440 across %d measurable files"
              % (med, len(devs)))
        if abs(med) > 15:
            print("   this instrument is not at A440 — Keys' Tune-on-Load will")
            print("   correct it per sample, or set Fine by hand")
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


def cmd_pick(args):
    """One representative file per INSTRUMENT out of a multisampled library.

    Real libraries are not flat. The Moog Voyager set is 1706 files that are
    really ~30 instruments x several velocity layers x ~25 notes, expressed
    three different ways: velocity as a numeric SUBFOLDER (Classic Lead/127),
    velocity as a filename suffix (...-C3-127.wav), and sub-patches nested a
    level down (Basics/Basic Square). Keys is single-zone today, so it stretches
    ONE sample across the keyboard — 25 notes per instrument only pays off once
    the multisampler lands. Until then: loudest velocity, note nearest a target.
    """
    root = args.folder
    groups = {}
    for dirpath, _dirs, names in os.walk(root):
        wavs = [n for n in names
                if os.path.splitext(n)[1].lower() in AUDIO_EXT and not n.startswith(".")]
        if not wavs:
            continue
        rel = os.path.relpath(dirpath, root)
        parts = [p for p in rel.split(os.sep) if p not in (".",)]
        # a purely numeric folder is a VELOCITY layer, not an instrument
        while parts and parts[-1].isdigit():
            parts.pop()
        key = "/".join(parts) if parts else os.path.basename(root)
        for n in wavs:
            note, hz = parse_name_note(os.path.join(dirpath, n), args.middle_c)
            vel = parse_name_vel(os.path.join(dirpath, n))
            groups.setdefault(key, []).append(
                {"path": os.path.join(dirpath, n), "note": note, "hz": hz, "vel": vel})

    _tn, target_hz = parse_name_note("x-%s-1.wav" % args.target, args.middle_c)
    if target_hz <= 0:
        raise SystemExit("could not parse --target %r as a note" % args.target)

    used, picks = set(), []
    for key in sorted(groups):
        items = groups[key]
        top = max(i["vel"] for i in items)
        cands = [i for i in items if i["vel"] == top and i["hz"] > 0]
        if not cands:
            cands = [i for i in items if i["vel"] == top]
        if not cands:
            continue
        best = min(cands, key=lambda i: abs(np.log2((i["hz"] or target_hz) / target_hz)))
        picks.append({"instrument": key, "src": best["path"], "note": best["note"],
                      "vel": top, "of": len(items),
                      "name": instrument_name(key, best["note"], used)})
    print("%-28s %-9s %-6s %-9s %s" % ("instrument", "note", "vel", "name", "of N files"))
    for p in picks:
        print("%-28s %-9s %-6s %-9s %d" % (p["instrument"][:28], p["note"] or "-",
                                           p["vel"], p["name"], p["of"]))
    os.makedirs(args.out, exist_ok=True)
    man = os.path.join(args.out, "manifest.json")
    for p in picks:
        p["dest"] = "usr/KEYS"
        p["accept"] = True
    with open(man, "w") as fh:
        json.dump(picks, fh, indent=2)
    print("\n%d instruments -> %s" % (len(picks), man))
    print("then: curate.py plan %s" % args.out)


def instrument_name(key, note, used):
    """Unique <= 8 char 8.3 base from an instrument path plus its note.

    26 instruments share prefixes — Simple Saw Pad / Simple Strings / Simple Sub
    all start "SIMPL", and Blitzkrieg / Blitzkrieg (clean) collide outright — so
    build from word initials plus the first word, then disambiguate numerically.
    """
    words = re.findall(r"[A-Za-z0-9]+", key.replace("/", " "))
    if not words:
        words = ["SMP"]
    n = re.sub(r"[^A-G#0-9]", "", (note or "").upper())
    room = max(1, 8 - len(n))
    if len(words) == 1:
        stub = words[0][:room].upper()
    else:
        initials = "".join(w[0] for w in words[1:])[:max(0, room - 3)].upper()
        stub = (words[0][:max(1, room - len(initials))] + initials).upper()[:room]
    cand, i = (stub + n)[:8], 1
    while cand in used:
        # shorten the STUB, never the note — overwriting the note's last
        # character turned a C#4 sample into "BASBPC#1", which Keys would then
        # parse as a C#1 octave hint. Actively misleading.
        tail = str(i)
        room = max(1, 8 - len(n) - len(tail))
        cand = (stub[:room] + tail + n)[:8]
        i += 1
    used.add(cand)
    return cand


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("folder")
    s.add_argument("--out", default="staged")
    s.add_argument("--recursive", action="store_true")
    s.add_argument("--limit", type=int, default=0)
    s.add_argument("--middle-c", type=int, default=3, dest="middle_c",
                   help="octave number this library gives middle C (3 = Yamaha, 4 = scientific)")
    s.set_defaults(fn=cmd_scan)
    k = sub.add_parser("pick", help="one file per instrument from a multisampled library")
    k.add_argument("folder")
    k.add_argument("--out", default="staged")
    k.add_argument("--target", default="C3", help="note to pick nearest to, in the library's own octave numbering")
    k.add_argument("--middle-c", type=int, default=3, dest="middle_c")
    k.set_defaults(fn=cmd_pick)
    p = sub.add_parser("plan")
    p.add_argument("out")
    p.set_defaults(fn=cmd_plan)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
