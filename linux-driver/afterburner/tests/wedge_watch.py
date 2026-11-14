#!/usr/bin/env python3
"""Sample afterburner BAR1 registers every 5ms while a run is in flight.
Usage: wedge_watch.py <duration_s>
Reads /sys/class/afterburner/afterburner0/{completion,h2c_status,c2h_status}
and prints t, completion(+0x2044), h2c(+0x44), c2h(+0x1044) on every change.
"""
import sys, time

BASE = "/sys/class/afterburner/afterburner0"

def rd(name):
    try:
        with open(f"{BASE}/{name}") as f:
            return int(f.read().strip(), 16)
    except Exception:
        return -1

dur = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
t0 = time.monotonic()
prev = None
print("t_s      completion h2c(+44) c2h(+1044)")
while time.monotonic() - t0 < dur:
    now = time.monotonic() - t0
    comp, h2c, c2h = rd("completion"), rd("h2c_status"), rd("c2h_status")
    cur = (comp, h2c, c2h)
    if cur != prev:
        print(f"{now:8.3f}  {comp:#010x}   {h2c:#010x}   {c2h:#010x}", flush=True)
        prev = cur
    time.sleep(0.005)
