#!/usr/bin/env python3
"""Final pre-doc checks: AB MC vf#0/vf#1 heads; sSubmitFrame trampoline exact LDR offset."""
import struct, capstone
d=open('rework/arm64e.bin','rb').read()
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)

for name, addr in (('AB MC vf#0', 0x4048), ('AB MC vf#1', 0xcac8)):
    print(f'=== {name} @0x{addr:x} ===')
    for i in md.disasm(d[addr:addr+0x10], addr):
        print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')
    print()

# sSubmitFrame trampoline — find the LDR [vptr, #imm] and PAC key
print('=== sSubmitFrame trampoline @0xcd7c (first 48 bytes) ===')
for i in md.disasm(d[0xcd7c:0xcd7c+0x30], 0xcd7c):
    print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')

# raw UC slot 308 entry (should be submitFrame body @0xd708, mid=0xcd42?)
q = struct.unpack_from('<Q', d, 0x14B98 + 8*308)[0]
print(f'\nraw UC slot 308 @0x{0x14B98+8*308:x}: tag={q>>48:02x} mid={(q>>32)&0xFFFF:04x} low={q&0xFFFFFFFF:08x}')
# and raw slots 306, 307 for context
for s in (305,306,307):
    q = struct.unpack_from('<Q', d, 0x14B98 + 8*s)[0]
    print(f'raw UC slot {s} @0x{0x14B98+8*s:x}: tag={q>>48:02x} mid={(q>>32)&0xFFFF:04x} low={q&0xFFFFFFFF:08x}')
