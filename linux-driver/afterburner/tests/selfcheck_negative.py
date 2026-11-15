#!/usr/bin/env python3
"""P-B1 negative test (FINDINGS §42): corrupt ONE stsz entry in a copy of the
test MOV and verify abplayer's startup offset self-check aborts with exit 2
BEFORE touching hardware.

Usage: selfcheck_negative.py [MOV] [abplayer-bin] [sample-idx]
Defaults: dev/testmedia/prorestest.mov at repo root, abplayer next to this file's parent dir.

Expected output: "offset self-check: sample <idx> bad ..." lines (the corruption
cascades through the rest of the chunk), then "== ABORT ... not touching
hardware" and exit code 2 with NO "[card]" line in the output."""
import os, struct, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
mov = sys.argv[1] if len(sys.argv) > 1 else \
    os.path.join(here, "..", "..", "..", "dev", "testmedia", "prorestest.mov")
player = sys.argv[2] if len(sys.argv) > 2 else \
    os.path.join(here, "..", "userspace", "abplayer")
idx = int(sys.argv[3]) if len(sys.argv) > 3 else 728

dst = "/tmp/badoff.mov"
data = bytearray(open(os.path.abspath(mov), "rb").read())

i = data.find(b"stsz")
assert i > 0, "no stsz box"
ss, count = struct.unpack(">II", data[i+8:i+16])
assert ss == 0 and count > idx, f"unexpected stsz (ss={ss}, count={count})"

pos = i + 16 + idx * 4
old, = struct.unpack(">I", data[pos:pos+4])
data[pos:pos+4] = struct.pack(">I", old + 100)
open(dst, "wb").write(data)
print(f"corrupted stsz[{idx}]: {old} -> {old+100} (wrote {dst})")

p = subprocess.run([player, dst, "--headless", "--no-pace"],
                   capture_output=True, text=True)
out = p.stdout + p.stderr
print(out.strip())
assert p.returncode == 2, f"expected exit 2, got {p.returncode}"
assert "not touching hardware" in out, "missing abort message"
assert "[card]" not in out, "HARDWARE WAS TOUCHED — self-check ran too late!"
print("PASS: self-check aborted pre-hardware with exit 2")
