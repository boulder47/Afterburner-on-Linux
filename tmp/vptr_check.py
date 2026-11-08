#!/usr/bin/env python3
"""Verify live UC vptr = 0x14BA8: pre-virtual entries, sGetPercent dispatch offset, fn@0xe6a4 head, allocator scan."""
import struct, capstone

d = open('rework/arm64e.bin','rb').read()
def q(off): return struct.unpack_from('<Q', d, off)[0]

print('=== pre-virtual entries at vtable start 0x14B98 ===')
for off in (0x14B98, 0x14BA0):
    print(f'@{off:05X}: {q(off):016x}')

print()
print('=== sGetPercent @0xcf90 full (to end of fn) ===')
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
for i in md.disasm(d[0xcf90:0xcf90+0x50], 0xcf90):
    print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')

print()
print('=== fn @0xe6a4 head (getPercent body candidate) ===')
for i in md.disasm(d[0xe6a4:0xe6a4+0x30], 0xe6a4):
    print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')

print()
print('=== scan __text for ADRP+ADD targeting page 0x14000 offsets 0xB98/0xBA8 (UC vptr setup) ===')
words = struct.unpack_from(f'<{len(d)//4}I', d, 0)
for pc in range(0x4000, 0xE83C, 4):
    w = words[pc//4]
    if (w & 0x9F000000) == 0x90000000:  # ADRP
        immlo = (w >> 29) & 3
        immhi = (w >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm >= 0x100000: imm -= 0x2000000
        page = (pc & ~0xFFF) + imm * 0x1000
        if page == 0x14000 and pc+4 < len(d):
            w2 = words[(pc+4)//4]
            # ADD immediate: sf=1 op=0 S=0 o0=0 o1=0 -> 0x91000000, imm12, Rn, Rd
            if (w2 & 0xFF800000) == 0x91000000:
                addimm = (w2 >> 10) & 0xFFF
                tgt = page + addimm
                if tgt in (0x14B98, 0x14BA8):
                    print(f'PC {pc:05X}: adrp x{(w&0x1F)}, #0x14000 ; add #{addimm:x} -> {tgt:05X}')
