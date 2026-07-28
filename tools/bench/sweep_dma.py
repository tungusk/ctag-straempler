#!/usr/bin/env python3
"""Sweep the I2S DMA geometry, measuring BOTH things that matter.

Deepening the buffer from 4x32 (2.9 ms) to 12x32 (8.7 ms) cut HTTP-induced
glitches by 93% on Synth — and made Keys audibly SCRATCHY, which I did not catch
because I validated the change on the wrong machine. Keys is the host that
streams its sample from PSRAM, so it is the one that must be checked.

Two metrics per configuration, both on KEYS:
  scratch  the material's own slew ceiling while a note sustains. A clean tone
           sits near 0.027; the scratchy 12x32 build read 0.142. This is a proxy
           for "the signal itself has acquired high-frequency junk".
  glitch   discontinuity rate while polling /status at the web UI's 2 Hz, which
           is the fault we are trying to fix.

Depth in milliseconds is count*len/44100, so 4x96 and 12x32 buy the same slack
with a third as many descriptors — worth knowing whether the harm comes from
DEPTH or from the number of descriptors.

  tools/bench/sweep_dma.py            # run the sweep (flashes repeatedly!)
"""
import os
import re
import subprocess
import threading
import time

import numpy as np

from detect import find_bursts
from rig import Rig, load_wav, get

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
I2S = os.path.join(REPO, "components", "drivers", "i2s_per.c")
CONFIGS = [(4, 32), (4, 96), (8, 32), (4, 192)]

ENV = ('export PATH="$HOME/.espressif/tools/xtensa-esp32-elf/'
       'esp-2021r2-patch3-8.4.0/xtensa-esp32-elf/bin:$PATH"; '
       'export IDF_PATH="$HOME/esp/esp-idf-v4.3"; . "$IDF_PATH/export.sh" >/dev/null 2>&1; ')


def set_dma(count, length):
    src = open(I2S).read()
    src = re.sub(r"\.dma_buf_count = \d+,", ".dma_buf_count = %d," % count, src)
    src = re.sub(r"\.dma_buf_len = \d+,", ".dma_buf_len = %d," % length, src)
    open(I2S, "w").write(src)


def build_flash():
    r = subprocess.run(ENV + "cd %s && idf.py build -DCMAKE_POLICY_VERSION_MINIMUM=3.5" % REPO,
                       shell=True, capture_output=True, text=True)
    if "Successfully created" not in r.stdout:
        raise RuntimeError("build failed:\n" + r.stdout[-1500:])
    r = subprocess.run("cd %s && ./tools/ota.sh 192.168.3.227" % REPO,
                       shell=True, capture_output=True, text=True)
    if '"ok":true' not in r.stdout:
        raise RuntimeError("flash failed:\n" + r.stdout[-800:])
    time.sleep(7)


def hold_note(rig, stop):
    while not stop.is_set():
        try:
            rig.trigger(2000)
        except Exception:                             # noqa: BLE001
            pass
        stop.wait(1.5)


def measure(rig, secs=8.0, poll=False):
    stop = threading.Event()
    th = threading.Thread(target=hold_note, args=(rig, stop), daemon=True)
    th.start()
    pstop = threading.Event()

    def poller():
        while not pstop.is_set():
            try:
                get("/status")
            except Exception:                         # noqa: BLE001
                pass
            pstop.wait(0.5)
    pt = None
    if poll:
        pt = threading.Thread(target=poller, daemon=True)
        pt.start()
    time.sleep(0.3)
    rig.capture_blocking(secs, "sweep_tmp.wav")
    stop.set()
    pstop.set()
    th.join(timeout=3)
    if pt:
        pt.join(timeout=3)
    x, sr = load_wav("sweep_tmp.wav")
    ev, thr, ceil = find_bursts(x, sr)
    return ceil, len(ev) / secs, float(np.max(np.abs(x)))


rig = Rig()
orig = open(I2S).read()
results = []
try:
    for count, length in CONFIGS:
        depth_ms = count * length / 44100.0 * 1000.0
        print("\n=== dma %d x %d = %d frames = %.1f ms ===" % (count, length, count * length, depth_ms),
              flush=True)
        set_dma(count, length)
        build_flash()
        st = rig.status()
        if st.get("machine") != "Keys":
            raise SystemExit("module is on %s, expected Keys" % st.get("machine"))
        scratch, _, pk = measure(rig, poll=False)
        _, glitch, _ = measure(rig, poll=True)
        results.append((count, length, depth_ms, scratch, glitch, pk))
        print("   scratch ceiling %.4f   glitch rate polled %.2f/s   peak %.3f"
              % (scratch, glitch, pk), flush=True)
finally:
    open(I2S, "w").write(orig)
    print("\nrestored i2s_per.c to its original contents (REFLASH to match)")

print("\n=== SUMMARY (Keys) ===")
print("%-10s %-9s %-10s %-12s %s" % ("dma", "depth ms", "scratch", "glitch/s", "verdict"))
for c, l, d, s, g, pk in results:
    v = "SCRATCHY" if s > 0.05 else ("good" if g < 0.15 else "ok")
    print("%-10s %-9.1f %-10.4f %-12.2f %s" % ("%dx%d" % (c, l), d, s, g, v))
