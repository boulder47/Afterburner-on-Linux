import capstone, struct, re
d = open('rework/arm64e.bin','rb').read()
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)

def raw_adrp(w, pc):
    """Confirmed layout: [31]=1,[30:29]=immlo,[28:24]=1_0000,[23:5]=immhi(19b),[4:0]=Rd"""
    assert (w & 0x9F000000) == 0x90000000, f'not an ADRP opcode: {w:08x}'
    immlo = (w >> 29) & 3
    immhi = (w >> 5) & 0x7FFFF          # 19 bits — ORIGINAL mask was correct
    rd = w & 0x1F
    imm = (immhi << 2) | immlo
    if imm & (1 << 20): imm -= (1 << 21)
    return ((pc >> 12) << 12) + imm * 0x1000, rd

mismatch = total = 0
for i in md.disasm(d[0x4000:0xd800], 0x4000):
    if i.mnemonic != 'adrp': continue
    w = struct.unpack_from('<I', d, i.address)[0]
    m = re.match(r'x(\d+),\s+#(0[xX][0-9a-fA-F]+|-?\d+)', i.op_str)
    if not m: continue
    rd_cs = int(m.group(1)); tgt_cs = int(m.group(2), 0)
    if tgt_cs >= (1 << 63): tgt_cs -= (1 << 64)
    tgt_raw, rd_raw = raw_adrp(w, i.address)
    total += 1
    if tgt_raw != tgt_cs or rd_raw != rd_cs:
        mismatch += 1
        print(f'MISMATCH @{i.address:#x}: capstone=({tgt_cs:#x}, x{rd_cs}) raw=({tgt_raw:#x}, x{rd_raw})')
print(f'ADRP cross-check: {total} instructions, {mismatch} mismatches')

# resolve the specific installResource string sites (the original 0xDB5 vs 0xDB6 question)
for pc in (0x8918, 0x8928):
    w = struct.unpack_from('<I', d, pc)[0]
    tgt, rd = raw_adrp(w, pc)
    for i in md.disasm(d[pc+4:pc+12], pc + 4):
        m = re.match(r'x(\d+),\s+x(\d+),\s+#(0[xX][0-9a-fA-F]+|\d+)', i.op_str)
        if m and int(m.group(2)) == rd:
            off = int(m.group(3), 0)
            final = tgt + off
            s = d[final:final+80].split(b'\x00')[0]
            print(f'@{pc:#x}: adrp x{rd},#{tgt:#x} + add #{off:#x} -> {final:#x}: {s!r}')
        else:
            print(f'@{pc:#x}: next insn not a matching ADD: {i.mnemonic} {i.op_str}')
