#!/usr/bin/env python3
"""Scan __const (and all non-text data) for runs of PAC-tagged fn ptrs = real vtables."""
import json, struct

data = open('../arm64e.bin', 'rb').read()
syms = {s['val']: s['name'] for s in json.load(open('syms_16.json'))}
imports = json.load(open('imports_map.json'))

TEXT_LO, TEXT_HI = 0x4000, 0xE83C

def decode(v):
    tag = (v >> 48) & 0xFFFF
    lo = v & 0xFFFFFFFF
    if tag == 0x8011 and TEXT_LO <= lo < TEXT_HI:
        return ('fn', syms.get(lo, f'fn@{lo:x}'))
    if tag == 0xc011 and str(int(lo)) in imports:
        return ('imp', imports[str(int(lo))]['name'])
    if v == 0:
        return ('zero', '')
    return None

# scan u64-aligned across whole file, skip __text
runs = []
cur = []
for off in range(0x1000, len(data) - 8, 8):
    if TEXT_LO <= off < TEXT_HI:
        if cur and len(cur) >= 3: runs.append(cur)
        cur = []
        continue
    v = struct.unpack_from('<Q', data, off)[0]
    d = decode(v)
    if d and d[0] in ('fn', 'imp'):
        cur.append((off, d))
    else:
        if len(cur) >= 3: runs.append(cur)
        cur = []
if len(cur) >= 3: runs.append(cur)

runs.sort(key=lambda r: -len(r))
print(f"total runs>=3: {len(runs)}")
for r in runs[:25]:
    lo, hi = r[0][0], r[-1][0] + 8
    print(f"\n=== run @0x{lo:x}-0x{hi:x} ({len(r)} entries) ===")
    for off, d in r:
        print(f"  {off:06x}: {d[0]:4s} {d[1]}")
