#!/usr/bin/env python3
"""Build PLT stub -> import name map for arm64e slice.

PLT stubs live just after __text (0xE83C+). Each is: adrp x16,xN; ldr x16,[x16,#off]; br x16
(optionally with pacibsp/autda preamble). The GOT slot index maps through the u32 array at
0x1806c (368 values) to an import name in imports_map.json.
"""
import json, struct

data = open("../arm64e.bin", "rb").read()
imports = json.load(open("imports_map.json"))  # dict idx -> {lib, weak, name}

# GOT base: find from a known stub (0xe884 -> GOT[0x14030] per FINDINGS §22)
GOT_BASE = 0x14000

def decode_stub(pc):
    """Return (got_addr, kind) for a PLT stub at pc, or None."""
    w0 = struct.unpack_from("<I", data, pc)[0]
    # adrp x16, #imm
    if (w0 >> 24) & 0x9F != 0x90:  # not adrp to x16... check properly below
        pass
    op0 = (w0 >> 24) & 0x9F
    rd = w0 & 0x1F
    if op0 == 0x10 and ((w0 >> 5) & 3) == 0:  # ADRP
        immlo = (w0 >> 29) & 3
        immhi = (w0 >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & (1 << 31):
            imm -= 1 << 32
        page = (pc & ~0xFFF) + imm
        w1 = struct.unpack_from("<I", data, pc + 4)[0]
        # ldr x16, [x16, #imm]
        if (w1 >> 24) == 0xF9 and ((w1 >> 5) & 3) == 0:  # LDR unsigned offset, 64-bit
            rt = w1 & 0x1F
            rn = (w1 >> 5) & 0x1F
            imm12 = (w1 >> 10) & 0xFFF
            if rd == 16 and rt == 16:
                return page + imm12 * 8, "ldr"
    return None

# scan the PLT region after __text end (0xE83C) up to data start (~0x10000)
stubs = {}
pc = 0xE83C
while pc < 0x10000:
    r = decode_stub(pc)
    if r:
        got, kind = r
        stubs[got] = pc
        # estimate stub length: scan until next adrp x16 or ret/br pattern; use 8-24 bytes
        pc += 8
    else:
        pc += 4

# map GOT slot -> import index via array @0x1806c (368 u32)
arr = struct.unpack_from("<368I", data, 0x1806c)
got_to_imp = {}
for i, v in enumerate(arr):
    # hypothesis: arr[i] is a GOT offset from base? or an index into something?
    got_to_imp[v] = i

out = {}
for got, pc in sorted(stubs.items()):
    off = got - GOT_BASE
    name = None
    if off in got_to_imp:
        idx = got_to_imp[off]
        if idx < len(imports):
            name = imports[idx]["name"]
    out[f"0x{pc:x}"] = {"got": f"0x{got:x}", "import_idx": got_to_imp.get(off), "name": name}

json.dump(out, open("plt_map.json", "w"), indent=1)
print(f"{len(stubs)} PLT stubs found")
named = [v for v in out.values() if v["name"]]
unnamed = [k for k, v in out.items() if not v["name"]]
print(f"named: {len(named)}, unnamed GOT offsets: {[hex(int(k,16)) for k in unnamed][:20]}")
