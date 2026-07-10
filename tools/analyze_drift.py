#!/usr/bin/env python3
"""Measure deck-click vs clock-pulse phase from a 2ch capture.
ch1 = deck audio (CLK120 clicks, accent every 4th beat), ch2 = clock pulses
(16ths = 4 per beat). Offset = click onset time minus nearest pulse time."""
import wave, sys
import numpy as np

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "drift_run1.wav")
sr = w.getframerate()
a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2)
deck, clk = a[:, 0].astype(np.float32), a[:, 1].astype(np.float32)

# bandpass the deck channel around the click tones (1/1.5 kHz) — kills the
# sub-30 Hz rumble the capture picks up at max gain
D = np.fft.rfft(deck)
fr = np.fft.rfftfreq(len(deck), 1 / sr)
D[(fr < 500) | (fr > 4000)] = 0
deck = np.fft.irfft(D, n=len(deck)).astype(np.float32)

print(f"{len(a)} frames @ {sr} Hz  deck peak {np.abs(deck).max():.0f} (bandpassed)  clk peak {np.abs(clk).max():.0f}")

def rising_edges(x, th_hi, th_lo, refractory):
    """indices where x crosses th_hi after having been below th_lo"""
    hi = x > th_hi
    edges = []
    armed = True
    last = -refractory
    for i in np.nonzero(hi)[0]:
        if i - last >= refractory:
            edges.append(i)
            last = i
    return np.array(edges)

# clock pulses: clean square-ish, threshold at half peak, refractory 60 ms
cpk = np.abs(clk).max()
pulses = rising_edges(clk, cpk * 0.5, cpk * 0.2, int(0.060 * sr))

# deck clicks: envelope onset. Rectify, threshold at 0.25*peak, refractory 300 ms
dpk = np.abs(deck).max()
clicks = rising_edges(np.abs(deck), dpk * 0.25, dpk * 0.1, int(0.300 * sr))

print(f"pulses: {len(pulses)}  median interval {np.median(np.diff(pulses))/sr*1000:.3f} ms")
print(f"clicks: {len(clicks)}  median interval {np.median(np.diff(clicks))/sr*1000:.3f} ms")

# offset of each click to nearest pulse (ms); pulses are 16ths so range ±62.5
offs = []
for c in clicks:
    j = np.searchsorted(pulses, c)
    cands = [pulses[k] for k in (j - 1, j) if 0 <= k < len(pulses)]
    off = min(((c - p) for p in cands), key=abs)
    offs.append(off / sr * 1000.0)
offs = np.array(offs)
t = clicks / sr

print(f"\nclick-to-pulse offset over {t[-1]-t[0]:.0f} s ({len(offs)} beats):")
print(f"  mean {offs.mean():+.2f} ms   std {offs.std():.2f} ms   min {offs.min():+.2f}   max {offs.max():+.2f}")
sl = np.polyfit(t, offs, 1)[0]
print(f"  linear slope {sl*60:+.3f} ms/min  (residual tempo slip)")
# wander: detrended peak-to-peak
det = offs - np.polyval(np.polyfit(t, offs, 1), t)
print(f"  detrended wander p-p {det.max()-det.min():.2f} ms  (std {det.std():.2f})")
# timeline in 30s buckets
print("\n  per-30s mean offset (ms):")
for b0 in range(0, int(t[-1]), 30):
    m = (t >= b0) & (t < b0 + 30)
    if m.sum():
        print(f"    {b0:3d}-{b0+30:3d}s: {offs[m].mean():+6.2f}  (n={m.sum()})")
