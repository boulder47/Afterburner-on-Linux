#!/usr/bin/env python3
"""Last micro-checks: UC instance vf#0 @0xcbe0 head; fn@0xe770 parent-class load."""
import struct, capstone
d=open('rework/arm64e.bin','rb').read()
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)

print('=== UC instance vf#0 @0xcbe0 (first 8 bytes) ===')
for i in md.disasm(d[0xcbe0:0xcbe0+8], 0xcbe0):
    print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')

print('\n=== fn@0xe770 (UC MC ctor) — first 40 insns, looking for OSMetaClassC2 args ===')
for i in md.disasm(d[0xe770:0xe770+0x60], 0xe770):
    print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')
