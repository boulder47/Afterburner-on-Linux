#!/usr/bin/env python3
"""Fast sampler: keep sysfs fds open, tight-loop read() of completion/h2c/c2h.
Usage: wedge_watch_fast.py <duration_s> [out_csv]
Run concurrently with a single-frame submit; captures the +0x2044 evolution."""
import sys, time

BASE = "/sys/class/afterburner/afterburner0"
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
out = open(sys.argv[2], "w") if len(sys.argv) > 2 else None

f_comp = open(f"{BASE}/completion", "rb")
f_h2c  = open(f"{BASE}/h2c_status", "rb")
f_c2h  = open(f"{BASE}/c2h_status", "rb")

t0 = time.monotonic()
last = None
n = 0
while time.monotonic() - t0 < dur:
    c = f_comp.read().strip()
    h = f_h2c.read().strip()
    k = f_c2h.read().strip()
    st = (c, h, k)
    if st != last:
        dt = (time.monotonic() - t0) * 1e6
        line = f"t={dt:9.1f}us comp={c.decode()} h2c={h.decode()} c2h={k.decode()}"
        print(line, flush=True)
        if out:
            out.write(line + "\n")
            out.flush()
        last = st
    n += 1

print(f"# {n} samples in {dur}s (~{dur*1e6/n:.0f}us/sample)", flush=True)
