#!/usr/bin/env python3
"""Capture a long stretch while Arlo plays, touching NOTHING else.

No REST traffic at all during the capture: polling /status at 4 Hz was measured
pushing peak block cost to 1495-1517 us of a 1450 us budget, and Arlo HEARD the
difference. If the rig disturbs the thing it is recording, the recording is
worthless. So this only runs sox.

  tools/bench/listen.py [seconds] [outfile]

LABELLING: when Arlo hears the artefact he plays THREE SHORT STABS immediately
after. That leaves a landmark in the waveform, and the event sits just before it
— far more reliable than correlating wall-clock guesses across a conversation.
"""
import sys

from rig import Rig

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
out = sys.argv[2] if len(sys.argv) > 2 else "listen.wav"

rig = Rig()
rig.check_device()
st = rig.status()          # one read BEFORE we start; nothing during
if st.get("recording"):
    raise SystemExit("ABORT: module is recording")
print("machine %s — capturing %.0f s to %s" % (st.get("machine"), secs, out))
print("PLAY NOW. When you hear the burst, play three short stabs straight after.")
rig.capture_blocking(secs, out)
print("done: %s" % out)
