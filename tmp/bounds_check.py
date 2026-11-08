#!/usr/bin/env python3
"""Final boundary checks: AB vtable head (0x40b8/0x40bc), AB terminator @0x14A70, UC tail 0x15590-0x15680, gap data."""
import struct, capstone

d = open('rework/arm64e.bin','rb').read()
def q(off): return struct.unpack_from('<Q', d, off)[0]
md = capstone.Cs(capspace := capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN) if False else capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)

print('=== AB vtable head: fn @0x40b8 and @0x40bc ===')
for i in md.disasm(d[0x40b8:0x40c8], 0x40b8):
    print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')

print()
print('=== AB terminator region @0x14A68-0x14A98 ===')
for off in range(0x14A68, 0x14A98, 8):
    print(f'@{off:05X}: {q(off):016x}')

print()
print('=== UC tail @0x15588-0x15688 (find table end before sMethods) ===')
for off in range(0x15588, 0x15688, 8):
    v = q(off)
    tag = (v >> 48) & 0xFFFF
    print(f'@{off:05X}: {v:016x} tag=0x{tag:04x}')

print()
print('=== AB allocator fn@0x4140 vptr store (confirm live AB vptr value) ===')
for i in md.disasm(d[0x4140:0x41c0], 0x4140):
    print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')
