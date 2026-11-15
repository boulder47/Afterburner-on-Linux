#!/usr/bin/env python3
"""Find all STR/STP writes to object offsets (default: 0xa8, 0xb0) across __text."""
import capstone, json, sys

data = open('../arm64e.bin', 'rb').read()
syms = json.load(open('syms_16.json'))
fns = sorted([(s['val'], s['name']) for s in syms if 0x4000 <= s['val'] < 0xE83C])

def fn_of(addr):
    name, start = "?", 0
    for v, n in fns:
        if v <= addr:
            name, start = n, v
        else:
            break
    return name, addr - start

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = True
targets = [int(t, 0) for t in sys.argv[1:]] or [0xa8]
for i, ins in enumerate(md.disasm(data[0x4000:], 0x4000)):
    m = ins.mnemonic
    if m not in ('str', 'stur', 'stp'):
        continue
    # parse offset from op_str like "x5, [x19, #0xa8]" or "[x29, #-0x58]"
    import re
    mo = re.search(r'#(0x[0-9a-f]+|-?\d+)', ins.op_str)
    if not mo:
        continue
    off = int(mo.group(1), 0)
    if off in targets:
        print(f'{ins.address:06x} [{fn_of(ins.address)[0]}+{fn_of(ins.address)[1]:x}] {m}\t{ins.op_str}')
