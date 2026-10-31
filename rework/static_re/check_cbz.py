import struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
data = open('../arm64e.bin','rb').read()
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
for i in md.disasm(data[0x9c5c:0xa28c], 0x9c5c):
    if 'cb' in i.mnemonic or i.mnemonic in ('b','bl'):
        print(f"{i.address:06x}: {i.mnemonic} {i.op_str}")
