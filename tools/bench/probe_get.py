#!/usr/bin/env python3
"""Does an ordinary GET /status disturb the audio, or only the trigger path?

~25% of REST requests produce an audible discontinuity, and a ping flood does
not, so it is httpd request handling rather than the radio. If a plain GET does
it too, then merely having the WEB UI OPEN — it polls /status every 500 ms —
glitches the audio, which matters to anyone using this thing.

Sound is identical in both conditions and the gate is refreshed at the same slow
rate; only the extra GET traffic differs. Level dropped to keep the interface
out of clipping, which manufactures discontinuities of its own.
"""
import threading, time
import numpy as np
from detect import find_bursts
from rig import Rig, load_wav, post, get

SECS, BLOCKS = 30.0, 3
rig = Rig(); keys = rig.params()
SY = {"eng":0,"note":48,"quant":True,"shape":0.0,"atk":0.01,"dec":0.05,"sus":1.0,
      "rel":0.2,"e2c":0.0,"gld":0,"cut":900.0,"res":0.2,"fold":0.0,"lvl":0.45,
      "fxsl":[2,0],"rv":2}
res={"quiet":[0,0.0],"polled":[0,0.0]}
stop=threading.Event()
def poller():
    while not stop.is_set():
        try: get("/status")
        except Exception: pass
        time.sleep(0.5)                      # exactly what the web UI does
try:
    post("/remote/machine?name=Synth"); time.sleep(4)
    rig.set_params(SY, verify=False); time.sleep(1.5)
    for blk in range(BLOCKS*2):
        cond="quiet" if blk%2==0 else "polled"
        wav="get_%s.wav"%cond
        proc=rig.capture(SECS,wav); time.sleep(0.6)
        th=None
        if cond=="polled":
            stop.clear(); th=threading.Thread(target=poller,daemon=True); th.start()
        t0=time.time()
        while time.time()-t0 < SECS-1.0:
            rig.trigger(2000); time.sleep(1.9)
        if th: stop.set(); th.join(timeout=3)
        try: proc.wait(timeout=SECS+20)
        except Exception: proc.kill(); time.sleep(1); continue
        x,sr=load_wav(wav); ev,thr,ceil=find_bursts(x,sr)
        res[cond][0]+=len(ev); res[cond][1]+=SECS
        print("  block %d  %-6s  %d events  (peak %.2f)"%(blk,cond,len(ev),float(np.max(np.abs(x)))),flush=True)
finally:
    stop.set()
    post("/remote/machine?name=Keys"); time.sleep(4)
    rig.set_params(keys, verify=False)
    print("\nback on Keys (smp %r)"%rig.params().get("smp"))
print("\n=== RESULT ===")
for c in ("quiet","polled"):
    n,s=res[c]; print("%-7s %2d events in %.0f s = %.3f/s"%(c,n,s,n/max(s,1)))
q=res["quiet"][0]/max(res["quiet"][1],1); p=res["polled"][0]/max(res["polled"][1],1)
print("\npolled/quiet %.1fx -> %s"%(p/max(q,1e-9),
  "ANY REQUEST does it — the web UI's own 500 ms poll glitches the audio"
  if p>1.8*q else "only the trigger path, not plain GETs"))
