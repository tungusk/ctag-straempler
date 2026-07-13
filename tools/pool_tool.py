#!/usr/bin/env python3
"""pool_tool — fill the Strämpler's SD sample pool over a USB card reader.

WiFi upload works but is slow; the module's own USB is serial-only. This runs
on the host: convert any audio ffmpeg/sox can read into the module's native
format (44.1 kHz stereo s16le interleaved .RAW + .JSN sidecar) and drop it
straight onto the mounted card. Batch mode swallows whole sample packs.

    pool_tool.py convert kick.wav loop.mp3 --card /Volumes/STRAMPLER
    pool_tool.py convert pack/*.wav --card /Volumes/STRAMPLER --prefix DR
    pool_tool.py convert chain.wav --ot chain.ot --card /Volumes/STRAMPLER
    pool_tool.py export /Volumes/STRAMPLER/usr/LOOP_0003.RAW -o loop3.wav
    pool_tool.py ls /Volumes/STRAMPLER

Names become 8.3-safe UPPERCASE bases (the module's FatFS world); collisions
get numeric tails. Files land in usr/ — when the pool grows folders
(REC_*/loops/docs, the planned layout), point --dest at the subfolder.
A --bpm stamp writes the deck-readable "bpm" key so a known-tempo loop syncs
without an analysis pass.
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys

RATE = 44100


def die(msg):
    print(f"pool_tool: {msg}", file=sys.stderr)
    sys.exit(1)


def find_converter():
    for c in ("ffmpeg", "sox"):
        if shutil.which(c):
            return c
    die("needs ffmpeg or sox on PATH (brew install ffmpeg)")


def safe_base(path, prefix=""):
    base = os.path.splitext(os.path.basename(path))[0]
    base = re.sub(r"[^A-Za-z0-9]", "", base).upper()
    base = (prefix.upper() + base)[:8]
    return base or "SAMPLE"


def uniquify(base, dest):
    cand, n = base, 1
    while os.path.exists(os.path.join(dest, cand + ".RAW")):
        tail = str(n)
        cand = base[: 8 - len(tail)] + tail
        n += 1
    return cand


def to_raw(conv, src, raw_path):
    if conv == "ffmpeg":
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", src,
               "-ar", str(RATE), "-ac", "2", "-f", "s16le", raw_path]
    else:
        cmd = ["sox", src, "-r", str(RATE), "-c", "2", "-b", "16",
               "-e", "signed-integer", "-t", "raw", raw_path]
    subprocess.run(cmd, check=True)


def cmd_convert(args):
    conv = find_converter()
    dest = os.path.join(args.card, args.dest) if args.card else args.dest
    if not os.path.isdir(dest):
        die(f"destination {dest} is not a directory (card mounted?)")
    srcs = []
    for pat in args.inputs:
        hits = glob.glob(pat)
        if not hits:
            die(f"no input matches {pat}")
        srcs += hits
    if args.ot and len(srcs) != 1:
        die("--ot pairs with exactly one input (the sliced chain)")
    for src in srcs:
        base = uniquify(safe_base(src, args.prefix), dest)
        raw = os.path.join(dest, base + ".RAW")
        to_raw(conv, src, raw)
        frames = os.path.getsize(raw) // 4
        sidecar = {"name": base, "description": os.path.basename(src),
                   "tags": ""}
        if args.bpm:
            sidecar["bpm"] = args.bpm     # deck-readable stamp: syncs unanalyzed
        with open(os.path.join(dest, base + ".JSN"), "w") as f:
            json.dump(sidecar, f)
        if args.ot:
            shutil.copyfile(args.ot, os.path.join(dest, base + ".OT"))
        secs = frames / RATE
        print(f"  {src} -> {base}.RAW  ({secs:.1f}s"
              f"{', bpm ' + str(args.bpm) if args.bpm else ''}"
              f"{', +.OT' if args.ot else ''})")
    print(f"{len(srcs)} file(s) in {dest}")


def cmd_export(args):
    if not os.path.isfile(args.raw):
        die(f"{args.raw} not found")
    out = args.output or os.path.splitext(os.path.basename(args.raw))[0] + ".wav"
    conv = find_converter()
    if conv == "ffmpeg":
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
               "-f", "s16le", "-ar", str(RATE), "-ac", "2", "-i", args.raw, out]
    else:
        cmd = ["sox", "-r", str(RATE), "-c", "2", "-b", "16",
               "-e", "signed-integer", "-t", "raw", args.raw, out]
    subprocess.run(cmd, check=True)
    print(f"{args.raw} -> {out}")


def cmd_ls(args):
    usr = os.path.join(args.card, "usr")
    if not os.path.isdir(usr):
        die(f"{usr} not found (card mounted?)")
    rows = []
    for f in sorted(os.listdir(usr)):
        if f.upper().endswith(".RAW"):
            sz = os.path.getsize(os.path.join(usr, f))
            jp = os.path.join(usr, os.path.splitext(f)[0] + ".JSN")
            bpm = ""
            if os.path.isfile(jp):
                try:
                    bpm = json.load(open(jp)).get("bpm", "")
                except Exception:
                    pass
            rows.append((f, sz / 4 / RATE, bpm))
    for f, secs, bpm in rows:
        print(f"  {f:14} {secs:8.1f}s  {bpm}")
    print(f"{len(rows)} samples")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("convert", help="audio file(s) -> RAW+JSN on the card")
    c.add_argument("inputs", nargs="+", help="audio files/globs (anything ffmpeg reads)")
    c.add_argument("--card", default="", help="mounted card root (e.g. /Volumes/STRAMPLER)")
    c.add_argument("--dest", default="usr", help="folder on the card (default usr)")
    c.add_argument("--prefix", default="", help="name prefix (e.g. DR -> DRKICK1)")
    c.add_argument("--bpm", type=float, help="stamp the sidecar with a known tempo")
    c.add_argument("--ot", help="copy this Octatrack .ot alongside (one input only)")
    c.set_defaults(fn=cmd_convert)
    e = sub.add_parser("export", help="RAW -> WAV (back to the DAW)")
    e.add_argument("raw")
    e.add_argument("-o", "--output")
    e.set_defaults(fn=cmd_export)
    l = sub.add_parser("ls", help="list the card's pool")
    l.add_argument("card")
    l.set_defaults(fn=cmd_ls)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
