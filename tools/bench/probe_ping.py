#!/usr/bin/env python3
"""Is it the WEB SERVER, or the radio/network stack?

~25% of REST requests to the module produce an audible discontinuity. That could
be the httpd task and its request handling stealing time, or it could be the
WiFi radio itself disturbing the audio path (this hardware's ADC is documented as
wifi-spike-sensitive, and the codebase already blames wifi for ADC outliers).

Ping exercises the radio and the lwIP stack but never reaches httpd. Sound is
held identical throughout; only the background traffic changes.
"""
import subprocess, time
import numpy as np
from detect import find_bursts
from rig import Rig, load_wav, post, IP

SECS, BLOCKS = 30.0, 3
rig = Rig(); keys = rig.params()
SY = {"eng":0,"note":48,"quant":True,"shape":0.0,"atk":0.01,"dec":0.05,"sus":1.0,
      "rel":0.2,"e2c":0.0,"gld":0,"cut":900.0,"res":0.2,"fold":0.0,"lvl":0.8,
      "fxsl":[2,0],"rv":2}
res={"quiet":[0,0.0],"ping":[0,0.0]}
try:
    post("/remote/machine?name=Synth"); time.sleep(4)
    rig.set_params(SY, verify=False); time.sleep(1.5)
    for blk in range(BLOCKS*2):
        cond = "quiet" if blk%2==0 else "ping"
        wav="ping_%s.wav"%cond
        proc=rig.capture(SECS,wav); time.sleep(0.6)
        flood=None
        if cond=="ping":
            # 20 pings/s, no httpd involvement at all
            flood=subprocess.Popen(["ping","-i","0.05","-q",IP],
                                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        t0=time.time()
        while time.time()-t0 < SECS-1.0:
            rig.trigger(2000); time.sleep(1.9)     # LOW post rate, same both ways
        if flood: flood.kill()
        try: proc.wait(timeout=SECS+20)
        except Exception: proc.kill(); time.sleep(1); continue
        x,sr=load_wav(wav); ev,thr,ceil=find_bursts(x,sr)
        res[cond][0]+=len(ev); res[cond][1]+=SECS
        print("  block %d  %-5s  %d events  (peak %.2f)"%(blk,cond,len(ev),float(np.max(np.abs(x)))),flush=True)
finally:
    post("/remote/machine?name=Keys"); time.sleep(4)
    rig.set_params(keys, verify=False)
    print("\nback on Keys (smp %r)"%rig.params().get("smp"))
print("\n=== RESULT ===")
for c in ("quiet","ping"):
    n,s=res[c]; print("%-6s %2d events in %.0f s = %.3f/s"%(c,n,s,n/max(s,1)))
q=res["quiet"][0]/max(res["quiet"][1],1); p=res["ping"][0]/max(res["ping"][1],1)
print("\nping/quiet ratio %.1fx -> %s"%(p/max(q,1e-9),
      "THE RADIO/NETWORK STACK (ping never touches httpd)" if p>1.8*q
      else "NOT plain traffic — points at httpd request handling"))
