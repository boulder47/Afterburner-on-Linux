#!/usr/bin/env python3
"""D0/D1 check: does AB vf#1 (fn@0x40bc) call e98c (OSObject::operator delete)? Does UC vf#1 (fn@cbe4)?"""
import struct, capstone
d=open('rework/arm64e.bin','rb').read()
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)

for name, start, end in (('AB vf#1 fn@0x40bc', 0x40bc, 0x40f8), ('UC vf#1 fn@cbe4', 0xcbe4, 0xcc14)):
    print(f'=== {name} ===')
    for i in md.disasm(d[start:end], start):
        mark = '   <<<< DELETE?' if (i.mnemonic=='bl' and 'e98c' in i.op_str) else ''
        print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}{mark}')
    print()
