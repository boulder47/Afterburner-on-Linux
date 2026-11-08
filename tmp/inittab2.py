#!/usr/bin/env python3
import struct
d=open('rework/arm64e.bin','rb').read()
def q(o): return struct.unpack_from('<Q',d,o)[0]
print('=== 0x14160-0x141E8 ===')
for off in range(0x14160, 0x141E8, 8):
    v=q(off); print(f'@{off:05X}: {v:016x} tag=0x{(v>>48)&0xFFFF:04x}')
print()
print('=== scan data 0x14000-0x15700 for low32==0xcacc (AB MC ctor ref) ===')
for off in range(0x14000, 0x15700, 8):
    v=q(off)
    if (v & 0xFFFFFFFF) == 0xcacc: print(f'@{off:05X}: {v:016x}')
