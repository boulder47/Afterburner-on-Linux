#!/usr/bin/env python3
"""Decode on-disk vtables: tag 0x8011 = local fn ptr, tag 0xc011 = import ordinal."""
import struct, json, sys

data = open('../arm64e.bin', 'rb').read()
imps = json.load(open('imports_map.json'))
syms = json.load(open('syms_16.json'))
sym_by_val = {}
for s in syms:
    sym_by_val.setdefault(s['val'], []).append(s['name'])

def dec(v):
    tag = (v >> 48) & 0xFFFF
    lo = v & 0xFFFFFFFF
    if tag == 0x8011:
        nm = sym_by_val.get(lo, [])
        return f'LOCAL 0x{lo:x} {nm[0][:40]}' if nm else f'LOCAL 0x{lo:x}'
    if tag == 0xc011 and lo < 368:
        e = imps.get(str(lo)) or imps.get(int(lo), None)
        nm = (e['name'] if isinstance(e, dict) else str(e))[:44]
        return f'IMP[{lo}] {nm}'
    return f'?tag{tag:x} lo={lo:x}'

base = int(sys.argv[1], 0)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 30
for i in range(n):
    off = base + i * 8
    if off + 8 > len(data):
        break
    v = struct.unpack_from('<Q', data, off)[0]
    print(f'+{i*8:03x}: {dec(v)}')
