#!/usr/bin/env python3
"""Final boundary checks for doc write-up."""
import struct, capstone
d=open('rework/arm64e.bin','rb').read()
def q(o): return struct.unpack_from('<Q',d,o)[0]
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)

print('=== UC instance table end region 0x15560-0x15598 ===')
for off in range(0x15560, 0x15598, 8):
    v=q(off); print(f'@{off:05X}: {v:016x} tag=0x{(v>>48)&0xFFFF:04x}')

print()
print('=== fn@0xcb1c head (AB MC dtor candidate) ===')
for i in md.disasm(d[0xcb1c:0xcb30], 0xcb1c): print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')
print()
print('=== fn@0xe7c0 head (UC MC dtor candidate) ===')
for i in md.disasm(d[0xe7c0:0xe7d4], 0xe7c0): print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')

print()
print('=== scan for entries with low32 == 0x10198 (AB MC object ptr refs) ===')
for off in range(0x14000, 0x15700, 8):
    v=q(off)
    if (v & 0xFFFFFFFF) == 0x10198: print(f'@{off:05X}: {v:016x}')

print()
print('=== scan for entries with low32 == 0x101C0 ===')
for off in range(0x14000, 0x15700, 8):
    v=q(off)
    if (v & 0xFFFFFFFF) == 0x101C0: print(f'@{off:05X}: {v:016x}')
