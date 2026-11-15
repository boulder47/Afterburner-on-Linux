#!/usr/bin/env python3
"""Reusable arm64e disassembler for rework/arm64e.bin (file offset == vmaddr).

Usage:
  python3 disasm.py <start_hex> [end_hex] [--no-annot]
Annotates ADRP+ADD/LDR pairs with resolved absolute addresses (symbols, vtable
entries, GOT imports, strings) and labels BL/BLR targets including PLT stubs.

Import mapping: on-disk __got[i] = 0x401_0000_0000_00NN where NN == i, so
GOT slot i <-> imports_map.json entry i (verified: first 54 names form the
coherent kext import set). PLT stubs in __stubs (0xE83C..0xEA40) load GOT slots.
"""
import sys, json, struct, os, re
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "arm64e.bin")
data = open(BIN, "rb").read()
syms = json.load(open(os.path.join(HERE, "syms_16.json")))
imports = json.load(open(os.path.join(HERE, "imports_map.json")))

sym_by_val = {}
for s in syms:
    sym_by_val.setdefault(s["val"], []).append(s["name"])

VTABLES = {0x141e8: "MAIN", 0x14a80: "META", 0x14b98: "UC", 0x15578: "UC_META"}
GOT_BASE, GOT_END = 0x14000, 0x141B0

def imp_name(idx):
    e = imports.get(str(idx))
    return e["name"] if e else None

# ---- precompute PLT stub map: pc -> import name ----
md0 = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
stub_map = {}  # stub pc -> (got_addr, slot, name)
pc = 0xE83C
while pc < 0xEA40:
    w0 = struct.unpack_from("<I", data, pc)[0]
    if (w0 >> 31) == 1 and ((w0 >> 24) & 0x1F) == 0x10 and (w0 & 31) == 16:  # ADRP x16
        immlo = (w0 >> 29) & 3
        immhi = (w0 >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & (1 << 31):
            imm -= 1 << 32
        page = (pc & ~0xFFF) + (imm << 12)
        w1 = struct.unpack_from("<I", data, pc + 4)[0]
        if ((w1 >> 23) & 0x1FF) == 0x1F2 and ((w1 >> 5) & 31) == 16:  # LDR x16,[x16,#imm*8]
            off = ((w1 >> 10) & 0xFFF) * 8
            got = page + off
            slot = (got - GOT_BASE) // 8 if GOT_BASE <= got < GOT_END else None
            stub_map[pc] = (got, slot, imp_name(slot) if slot is not None else None)
    pc += 4

def resolve(addr):
    """Return annotation string for an absolute address, or ''."""
    if addr in sym_by_val:
        return ",".join(sym_by_val[addr])
    # GOT import slot?
    if GOT_BASE <= addr < GOT_END and (addr - GOT_BASE) % 8 == 0:
        i = (addr - GOT_BASE) // 8
        nm = imp_name(i)
        return f"GOT[{i}] {nm}" if nm else f"GOT[{i}]"
    # vtable entry?
    for base, tag in VTABLES.items():
        if base <= addr < base + 0x2000:
            off = addr - base
            if off >= 0x10 and (off - 0x10) % 8 == 0:
                vf = (off - 0x10) // 8
                v = struct.unpack_from("<Q", data, addr)[0]
                top = (v >> 56) & 0xFF
                lo32 = v & 0xFFFFFFFF
                if top == 0x80 and 0x4000 <= lo32 < 0xE83C:
                    nm = f" ({sym_by_val[lo32]})" if lo32 in sym_by_val else ""
                    return f"{tag}[vf#{vf}] -> fn@{lo32:x}{nm}"
                elif top == 0xC0 and lo32 < len(imports):
                    return f"{tag}[vf#{vf}] -> IMP[{lo32}] {imp_name(lo32)}"
    # string?
    end = data.find(b"\x00", addr)
    if 0 <= end - addr < 128:
        chunk = data[addr:end]
        if all(32 <= b < 127 for b in chunk):
            return f'str "{chunk.decode()[:60]}"'
    # data word
    if addr % 4 == 0 and addr + 4 <= len(data):
        w = struct.unpack_from("<I", data, addr)[0]
        return f"dword {w:#x}"
    return ""

def bl_annot(tgt):
    """Annotation for a b/bl target."""
    if tgt in sym_by_val:
        return ",".join(sym_by_val[tgt])
    if tgt in stub_map:
        got, slot, nm = stub_map[tgt]
        return f"PLT->GOT[{slot}] {nm}" if nm else f"PLT->{got:#x}"
    return ""

def disasm(start, end=None, annot=True):
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    code = data[start:end] if end else data[start:]
    insns = list(md.disasm(code, start))
    out = []
    last_adrp = None  # (reg, page)
    for insn in insns:
        line = f"{insn.address:08x}: {insn.mnemonic:<5} {insn.op_str}"
        mn, ops = insn.mnemonic.lower(), insn.op_str
        if annot and mn == "adrp":
            reg = ops.split(",")[0].strip()
            m = re.search(r"#(0x[0-9a-f]+|-?\d+)", ops)
            page = int(m.group(1), 0) if m else None  # capstone prints absolute target
            last_adrp = (reg, page)
        elif annot and mn in ("add", "ldr", "str") and last_adrp:
            reg, page = last_adrp
            if page is not None and ops.startswith(reg):
                imm, ok = 0, False
                parts = ops.split(",")
                if len(parts) >= 2:
                    p1 = parts[1].strip()
                    if p1.startswith("#"):
                        try:
                            imm = int(p1.lstrip("#"), 0); ok = True
                        except ValueError:
                            pass
                    elif p1.startswith("[") and "]" in p1:
                        inner = p1[p1.index("[")+1:p1.rindex("]")]
                        if "#" in inner:
                            try:
                                imm = int(inner.split("#")[1].split(",")[0], 0); ok = True
                            except (ValueError, IndexError):
                                pass
                if ok and page + imm < len(data) + 0x10000:
                    ann = resolve(page + imm)
                    if ann:
                        line += f"   ; {page+imm:#x} {ann}"
        last_adrp = None
        if mn in ("b", "bl"):
            tgt = int(ops.lstrip("#"), 16)
            a = bl_annot(tgt)
            if a:
                line += f"   ; {a}"
        out.append(line)
    return "\n".join(out)

if __name__ == "__main__":
    start = int(sys.argv[1], 0)
    end = int(sys.argv[2], 0) if len(sys.argv) > 2 else None
    print(disasm(start, end))
