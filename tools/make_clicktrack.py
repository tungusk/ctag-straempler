#!/usr/bin/env python3
"""Render mathematically exact click tracks in the deck's native RAW format
(44.1 kHz stereo interleaved s16le) for tempo-detection acceptance testing.

Beat positions are computed in double precision and rounded per-click, so
the tempo carries no accumulating error: frame(n) = round(n * 44100*60/bpm).
Every 4th beat is accented (higher pitch, louder) so bar alignment against
an external clock is audible by ear.

Usage: make_clicktrack.py [outdir]           # writes CLK120.RAW, CLK1234.RAW
       make_clicktrack.py outdir BPM NAME    # one custom track
"""
import math, struct, sys

SR = 44100
DUR_S = 270              # ~4.5 min: long enough for the k=256 refinement rung
                         # (needs 256 beats in half the track) and still under
                         # the deck's 5-min analysis cap

def render(bpm, path):
    frames_per_beat = SR * 60.0 / bpm
    n_frames = DUR_S * SR
    buf = bytearray(n_frames * 4)          # stereo s16le, silence
    click_len = int(0.008 * SR)            # 8 ms window, ~1.5 ms decay tau
    n_beats = int(n_frames / frames_per_beat)
    for n in range(n_beats):
        start = round(n * frames_per_beat)
        accent = (n % 4 == 0)
        freq = 1500.0 if accent else 1000.0
        amp = 0.85 if accent else 0.5
        for i in range(click_len):
            f = start + i
            if f >= n_frames:
                break
            s = amp * math.sin(2 * math.pi * freq * i / SR) * math.exp(-i / (0.0015 * SR))
            v = max(-32767, min(32767, int(s * 32767)))
            struct.pack_into('<hh', buf, f * 4, v, v)
    with open(path, 'wb') as fh:
        fh.write(buf)
    print(f"{path}: {bpm} BPM, {n_frames} frames, {frames_per_beat:.4f} frames/beat, {n_beats} clicks")

if __name__ == '__main__':
    outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
    if len(sys.argv) > 3:
        render(float(sys.argv[2]), f"{outdir}/{sys.argv[3]}.RAW")
    else:
        render(120.0, f"{outdir}/CLK120.RAW")    # integer 22050 frames/beat
        render(123.4, f"{outdir}/CLK1234.RAW")   # fractional frames/beat
