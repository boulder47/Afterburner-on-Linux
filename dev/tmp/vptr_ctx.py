#!/usr/bin/env python3
"""Context around each ADRP+ADD->0x14B98 site: what happens with x16 after? Also dump 0x14B78-0x14BB0."""
import struct, capstone

d = open('rework/arm64e.bin','rb').read()
def q(off): return struct.unpack_from('<Q', d, off)[0]

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)

print('=== data 0x14B78-0x14BB0 (gap between metaclass table end and UC vtable?) ===')
for off in range(0x14B78, 0x14BB0, 8):
    print(f'@{off:05X}: {q(off):016x}')

print()
for site in (0xcb84, 0xcbb8, 0xcc94, 0xccf0, 0xcd48):
    print(f'=== context @ {site-0x20} .. {site+0x30} ===')
    for i in md.disasm(d[site-0x20:site+0x30], site-0x20):
        mark = ' <==' if i.address == site else ''
        print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}{mark}')
    print()
