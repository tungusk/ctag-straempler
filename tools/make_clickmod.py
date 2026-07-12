#!/usr/bin/env python3
"""Generate a minimal ProTracker MOD that clicks on every beat — the tracker
machine's tempo-sync measurement fixture (counterpart of make_clicktrack.py).

One 4-channel M.K. pattern, a click on channel 1 every 4 rows (= every beat
at the tracker's 4-rows-per-beat convention), default speed 6 / tempo 125.
Usage: make_clickmod.py [OUT.MOD]
"""
import struct, sys

out = sys.argv[1] if len(sys.argv) > 1 else "CLICKTRK.MOD"

# click sample: 8-bit signed, sharp bipolar impulse with a short decay tail
click = bytearray()
for i in range(8):  click.append(127)
for i in range(8):  click.append(0x80)          # -128
for i in range(48): click.append((127 - i * 2) & 0xFF if i < 32 else 0)
if len(click) % 2: click.append(0)

b = bytearray()
b += b"CLICKTRACK".ljust(20, b"\x00")                     # title
# sample 1 header
b += b"click".ljust(22, b"\x00")
b += struct.pack(">H", len(click) // 2)                    # length in words
b += bytes([0, 64])                                        # finetune, volume
b += struct.pack(">HH", 0, 1)                              # no loop
# samples 2..31 empty
for _ in range(30):
    b += b"\x00" * 22 + struct.pack(">H", 0) + bytes([0, 64]) + struct.pack(">HH", 0, 1)
b += bytes([1, 127])                                       # song length, restart
b += bytes([0]) * 128                                      # order: pattern 0
b += b"M.K."
# pattern 0: 64 rows x 4 ch; click on ch1 every 4th row, period 214 (~C-3)
PERIOD = 214
for row in range(64):
    for ch in range(4):
        if ch == 0 and row % 4 == 0:
            smp = 1
            b += bytes([ (smp & 0xF0) | ((PERIOD >> 8) & 0x0F), PERIOD & 0xFF,
                         ((smp & 0x0F) << 4) | 0, 0 ])
        else:
            b += bytes([0, 0, 0, 0])
b += click

open(out, "wb").write(b)
print(f"{out}: {len(b)} bytes, click every 4 rows (1 beat), speed 6 / tempo 125")
