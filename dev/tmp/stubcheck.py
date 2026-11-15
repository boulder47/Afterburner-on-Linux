#!/usr/bin/env python3
"""Verify: AB instance local slots that are claimed forwarding stubs; UC MC vf#1 @0xe76c."""
import struct, capstone
d=open('rework/arm64e.bin','rb').read()
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)

targets = [
    ('AB vf#22', 0xc984), ('AB vf#132', 0xca68), ('AB vf#133', 0xca80),
    ('AB vf#198', 0xca98), ('AB vf#199', 0xcab0),
    ('UC MC vf#1 (D0?)', 0xe76c), ('UC MC vf#21 allocator head', 0xcc68),
]
for name, addr in targets:
    print(f'=== {name} @0x{addr:x} ===')
    for i in md.disasm(d[addr:addr+0x14], addr):
        print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')
    print()
