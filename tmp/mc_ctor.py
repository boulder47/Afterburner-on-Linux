#!/usr/bin/env python3
"""fn@0xcacc head (AB metaclass ctor target), fn@0xe770 head (UC metaclass ctor), + resolve e8fc/e9b0/e9c8/e998 from imports_map."""
import struct, capstone, json

d = open('rework/arm64e.bin','rb').read()
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)

print('=== fn@0xcacc head (AB metaclass ctor) ===')
for i in md.disasm(d[0xcacc:0xcacc+0x50], 0xcacc):
    print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')

print()
print('=== fn@0xe770 head (UC metaclass ctor) ===')
for i in md.disasm(d[0xe770:0xe770+0x50], 0xe770):
    print(f'{i.address:06x}: {i.mnemonic:<8} {i.op_str}')

print()
im = json.load(open('rework/static_re/imports_map.json'))
# find entries for stub addresses e8fc, e9b0, e9c8, e998 (vmaddr or file offset?)
def show(addr):
    a = addr & 0xFFFFFFFF
    hits = []
    for k, v in im.items():
        s = json.dumps(v) if not isinstance(v, str) else v
        if f'{a:08x}' in s.lower() or f'0x{a:x}' in s.lower():
            hits.append((k, v))
    return hits

for name, addr in (('e998', 0xe998), ('e9b0', 0xe9b0), ('e8fc', 0xe8fc), ('e9c8', 0xe9c8)):
    print(f'--- {name} @0x{addr:x} ---')
    h = show(addr)
    if not h:
        # try GOT-based lookup: read the stub itself (ADRP+LDR pattern) and resolve via GOT
        w0 = struct.unpack_from('<I', d, addr)[0]
        print(f'  no direct map hit; stub word0={w0:08x}')
    for k, v in h[:3]:
        print(f'  {k}: {v}')
