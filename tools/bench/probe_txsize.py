#!/usr/bin/env python3
"""httpd's CPU work, or the size of the WiFi transmit burst?

A small ping flood (64 B replies) does not glitch the audio; HTTP requests
(several hundred bytes of response) do, ~28% of the time. Those differ in two
ways: httpd's request handling, and the size of the transmit burst. Large pings
separate them — same lwIP path as a small ping, no httpd at all, but a TX burst
comparable to an HTTP response.
"""
import subprocess, time
import numpy as np
from detect import find_bursts
from rig import Rig, load_wav, post, IP

SECS, BLOCKS = 30.0, 3
rig = Rig(); keys = rig.params()
SY = {"eng":0,"note":48,"quant":True,"shape":0.0,"atk":0.01,"dec":0.05,"sus":1.0,
      "rel":0.2,"e2c":0.0,"gld":0,"cut":900.0,"res":0.2,"fold":0.0,"lvl":0.45,
      "fxsl":[2,0],"rv":2}
res={"quiet":[0,0.0],"bigping":[0,0.0]}
try:
    post("/remote/machine?name=Synth"); time.sleep(4)
    rig.set_params(SY, verify=False); time.sleep(1.5)
    for blk in range(BLOCKS*2):
        cond="quiet" if blk%2==0 else "bigping"
        wav="tx_%s.wav"%cond
        try:
            rig.capture_blocking(SECS,wav)
        except Exception as e:
            print("  block %d capture failed: %s"%(blk,e)); break
        fl=None
        if cond=="bigping":
            # ~1400 B payload at 2/s — same rate as the /status poll that DID glitch
            fl=subprocess.Popen(["ping","-i","0.5","-s","1400","-q",IP],
                                stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        import threading
        stop=threading.Event()
        def hold():
            while not stop.is_set():
                try: rig.trigger(2000)
                except Exception: pass
                stop.wait(1.9)
        th=threading.Thread(target=hold,daemon=True); th.start()
        if fl: fl.kill()
        stop.set(); th.join(timeout=3)
        x,sr=load_wav(wav); ev,thr,ceil=find_bursts(x,sr)
        res[cond][0]+=len(ev); res[cond][1]+=SECS
        print("  block %d  %-8s %d events  (peak %.2f)"%(blk,cond,len(ev),float(np.max(np.abs(x)))),flush=True)
finally:
    post("/remote/machine?name=Keys"); time.sleep(4)
    rig.set_params(keys, verify=False)
    print("\nback on Keys (smp %r)"%rig.params().get("smp"))
print("\n=== RESULT ===")
for c in ("quiet","bigping"):
    n,s=res[c]; print("%-8s %2d events in %.0f s = %.3f/s"%(c,n,s,n/max(s,1)))
q=res["quiet"][0]/max(res["quiet"][1],1); p=res["bigping"][0]/max(res["bigping"][1],1)
print("\nbigping/quiet %.1fx -> %s"%(p/max(q,1e-9),
  "TX BURST SIZE — nothing to do with httpd" if p>1.8*q
  else "httpd's own handling, not the transmit burst"))
