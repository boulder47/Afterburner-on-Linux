#!/usr/bin/env python3
"""Enumerate __stubs PLT trampolines and map each to its GOT slot + import ordinal."""
import struct, json, re
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

data = open("../arm64e.bin", "rb").read()
md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
code = data[0xE83C:0xEA40]

stubs = []
cur = None
for i in md.disasm(code, 0xE83C):
    if i.mnemonic == "adrp" and i.op_str.startswith("x16"):
        if cur:
            stubs.append(cur)
        m = re.search(r"#(0x[0-9a-f]+|-?\d+)", i.op_str)
        page = int(m.group(1), 0)  # capstone prints the absolute ADRP target
        cur = {"pc": hex(i.address), "page": page}
    elif cur is not None and i.mnemonic == "ldr" and i.op_str.startswith("x16"):
        m = re.search(r"#(0x[0-9a-f]+)", i.op_str)
        off = int(m.group(1), 16) if m else 0
        cur["got"] = page + off
    elif cur is not None and i.mnemonic in ("br", "ret"):
        stubs.append(cur)
        cur = None
if cur:
    stubs.append(cur)

print(f"{len(stubs)} stubs")
for s in stubs:
    got = s.get("got")
    slot = (got - 0x14000) // 8 if got and 0x14000 <= got < 0x141B0 else None
    print(f"{s['pc']} -> GOT {hex(got) if got else '?':>8} slot={slot}")

json.dump(stubs, open("stubs.json", "w"), indent=1)
