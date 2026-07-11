#!/usr/bin/env python3
"""Octatrack .ot slice-sidecar tool — create test fixtures and inspect files.

  ot_tool.py dump FILE.OT                    show fields + verify checksum
  ot_tool.py make FILE.OT LEN N [BPM]        even grid: N slices over LEN frames
  ot_tool.py make FILE.OT LEN p1,p2,... [BPM]  explicit slice-start frames

Layout (832 bytes, big-endian) matches components/machine_slicer/slicer_ot.c.
Use `dump` to diff a module-exported file against one from a real Octatrack.
"""
import struct, sys

SIZE = 832
HEADER = b"FORM\x00\x00\x00\x00DPS1SMPA"

def checksum(b):
    return sum(b[0x10:0x33E]) & 0xFFFF

def dump(path):
    b = open(path, "rb").read()
    print(f"{path}: {len(b)} bytes")
    if len(b) != SIZE: print("  !! wrong size"); return 1
    print(f"  header: {'OK' if b[:16] == HEADER else 'BAD ' + b[:16].hex()}")
    print(f"  unknown[7] @0x10: {b[0x10:0x17].hex()}")
    tempo, trimlen, looplen, stretch, loop = struct.unpack(">5I", b[0x17:0x2B])
    gain, quant = struct.unpack(">HB", b[0x2B:0x2E])
    tstart, tend, lpoint = struct.unpack(">3I", b[0x2E:0x3A])
    count = struct.unpack(">I", b[0x33A:0x33E])[0]
    want = struct.unpack(">H", b[0x33E:0x340])[0]
    have = checksum(b)
    print(f"  tempo {tempo} (= {tempo/24:.2f} bpm)   trimLen {trimlen} loopLen {looplen} (bars*100)")
    print(f"  stretch {stretch}  loop {loop}  gain 0x{gain:x}  quantize 0x{quant:x}")
    print(f"  trimStart {tstart}  trimEnd {tend}  loopPoint {lpoint}")
    print(f"  slices: {count}")
    for i in range(min(count, 64)):
        s, e, lp = struct.unpack(">3I", b[0x3A + i*12 : 0x3A + i*12 + 12])
        lps = "none" if lp == 0xFFFFFFFF else str(lp)
        print(f"    {i:2d}: start {s:>9}  end {e:>9}  loop {lps}")
    print(f"  checksum: file 0x{want:04x} computed 0x{have:04x} {'OK' if want == have else 'MISMATCH'}")
    return 0 if want == have else 1

def make(path, length, starts, bpm):
    n = len(starts)
    assert 1 <= n <= 64, "1..64 slices"
    b = bytearray(SIZE)
    b[:16] = HEADER
    b[0x10:0x17] = bytes([0, 0, 0, 0, 0, 2, 0])
    beats = length * bpm / (44100 * 60)
    bars100 = max(25, round(beats / 4 * 100))
    struct.pack_into(">5I", b, 0x17, round(bpm * 24), bars100, bars100, 0, 0)
    struct.pack_into(">HB", b, 0x2B, 0x30, 0xFF)
    struct.pack_into(">3I", b, 0x2E, 0, length, 0)
    bounds = list(starts) + [length]
    for i in range(n):
        struct.pack_into(">3I", b, 0x3A + i*12, bounds[i], bounds[i+1], 0xFFFFFFFF)
    struct.pack_into(">I", b, 0x33A, n)
    struct.pack_into(">H", b, 0x33E, checksum(b))
    open(path, "wb").write(b)
    print(f"{path}: {n} slices over {length} frames @ {bpm} bpm")

if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "dump":
        sys.exit(dump(sys.argv[2]))
    if len(sys.argv) >= 5 and sys.argv[1] == "make":
        length = int(sys.argv[3])
        spec = sys.argv[4]
        bpm = float(sys.argv[5]) if len(sys.argv) > 5 else 120.0
        if "," in spec or not spec.isdigit() or int(spec) > 64:
            starts = [int(x) for x in spec.split(",")]
        else:
            n = int(spec)
            starts = [round(i * length / n) for i in range(n)]
        make(sys.argv[2], length, starts, bpm)
        sys.exit(0)
    print(__doc__)
    sys.exit(2)
