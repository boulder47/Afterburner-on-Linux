import json, struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

import os
data = open(os.path.join(os.path.dirname(__file__), '..', 'arm64e.bin'),'rb').read()

def dec(base, off):
    v = struct.unpack_from('<Q', data, base+off)[0]
    tag = (v >> 48) & 0xFFFF
    lo = v & 0xFFFFFFFF
    if tag == 0x8011: return f"local {lo:#x}"
    if tag == 0xc011: return f"import[{lo}]"
    return f"raw {v:#x}"

print("== AB vtable @0x141E8 ==")
for off in (0x1e8, 0x1f0, 0x1f8, 0x200, 0x300, 0x308, 0x310):
    print(f"+{off:03x} vf#{off//8-2}: {dec(0x141E8, off)}")

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
text = data[0x4000:0xE83C]
hits9f9c, hits9d50 = [], []
for i in md.disasm(text, 0x4000):
    if i.mnemonic in ('bl','b'):
        t = i.op_str.strip().lstrip('#')
        try: tv = int(t,16)
        except ValueError: continue
        if tv == 0x9f9c: hits9f9c.append(i.address)
print("callers of 0x9f9c:", [hex(h) for h in hits9f9c])
