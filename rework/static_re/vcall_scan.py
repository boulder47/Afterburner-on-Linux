#!/usr/bin/env python3
"""Scan __text for virtual call sites: PAC-check + add x8,x16,#off + ldr x9,[x16,#off] + blraa.
Groups by (object-register, vtable-offset) and prints arg setup before each call."""
import struct, re, json
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

data = open("../arm64e.bin", "rb").read()
syms = json.load(open("syms_16.json"))
sym_by_val = {}
for s in syms:
    sym_by_val.setdefault(s["val"], []).append(s["name"])

# function boundaries from symtab (sorted by val)
fns = sorted({s["val"]: ",".join(sym_by_val[s["val"]]) for s in syms if 0x4000 <= s["val"] < 0xE83C}.items())

def fn_of(pc):
    cur = None
    for v, n in fns:
        if v > pc:
            break
        cur = (v, n)
    return cur

md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
TEXT_START, TEXT_END = 0x4000, 0xE83C
insns = list(md.disasm(data[TEXT_START:TEXT_END], TEXT_START))

# find blraa sites; walk back for 'ldr x9,[x16,#off]' then 'add x8,x16,#off'
sites = []
for i in range(4, len(insns)):
    c = insns[i]
    if c.mnemonic != "blraa" or not re.match(r"x9,\s*x8", c.op_str):
        continue
    off, objreg = None, "?"
    for j in range(i-1, max(-1, i-6), -1):
        ins = insns[j]
        m2 = re.match(r"x9,\s*\[x16,\s*#?(0x[0-9a-f]+)\]", ins.op_str)
        if ins.mnemonic == "ldr" and m2:
            off = int(m2.group(1), 16)
            break
    if off is None:
        continue
    # object reg: nearest 'ldr x16,[Xn]' before the add
    for j in range(i-1, max(-1, i-15), -1):
        ins = insns[j]
        m3 = re.match(r"x16,\s*\[(x\d+|xzr)\]", ins.op_str)
        if ins.mnemonic == "ldr" and m3:
            objreg = m3.group(1)
            break
    # args set in the ~10 instructions before (mov/add/sub/ldr to x0-x7)
    args = []
    for k in range(max(0, i-10), i):
        ins = insns[k]
        if ins.mnemonic in ("mov", "add", "sub", "ldr") and re.match(r"x[0-7],", ins.op_str):
            args.append(f"{ins.address:04x}:{ins.mnemonic} {ins.op_str}")
    res = fn_of(c.address)
    v, n = res if res else (0, "?")
    sites.append((c.address, off, objreg, v, n or "?", args))

# group by (objreg, offset)
from collections import defaultdict
groups = defaultdict(list)
for pc, off, objreg, fv, fn, args in sites:
    groups[(off or 0)].append((pc, objreg, fn, args))

print(f"total vcall sites: {len(sites)}")
json.dump([{"pc": hex(pc), "off": off, "objreg": o, "fn": f, "args": a} for pc, off, o, fv, f, a in sites],
          open("vcall_sites.json", "w"), indent=1)
print()
for off in sorted(groups):
    g = groups[off]
    fns_used = {}
    for pc, objreg, fn, args in g:
        short = fn.split("::")[-1][:40] if "::" in fn else fn[:40]
        fns_used.setdefault(short, 0)
        fns_used[short] += 1
    print(f"=== vtable +{off:#x}  ({len(g)} sites)")
    for short, cnt in sorted(fns_used.items(), key=lambda x: -x[1])[:8]:
        print(f"      {cnt:3d}x  {short}")
