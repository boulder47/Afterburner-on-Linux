#!/usr/bin/env python3
"""Settle trampoline identity: disasm sMethods tramp targets (0xcd7c..) vs UC slot 308-314 fns, + externalMethod body."""
import capstone

d = open('rework/arm64e.bin','rb').read()
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
md.detail = True

def dis(lo, hi, label):
    print(f'===== {label} ({lo:#x}-{hi:#x}) =====')
    code = d[lo:hi]
    for i in md.disasm(code, lo):
        print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')
    print()

# sMethods rec tramp targets (per UC_METHODS §1)
dis(0xcd7c, 0xcf98+4, 'sMethods trampoline region @0xcd7c-0xcfc8')
# UC slot 308-314 local fns
for i, addr in enumerate([0xd708, 0xdcc4, 0xdd9c, 0xe2a4, 0xe36c, 0xe4c0]):
    dis(addr, addr+0x30, f'UC slot {308+i} fn @0x{addr:x}')
