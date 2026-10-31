import struct, json, sys
data = open('../arm64e.bin','rb').read()

def vt_entry(base, off):
    val = struct.unpack_from('<Q', data, base + off)[0]
    tag = (val >> 48) & 0xFFFF
    low32 = val & 0xFFFFFFFF
    return hex(val), '0x%04x' % tag, hex(low32)

offs_self = [0x1f0, 0x288, 0x290, 0x3a8, 0x5e0, 0x630, 0x7c0, 0x7d8, 0x9b8]
offs_gate = [0x170, 0x178, 0x1e8, 0x200, 0x208, 0x210]
offs_prov = [0x278, 0x2c8, 0x738, 0x8f8, 0x908, 0x938]

print('=== AB vtable (self) ===')
for off in offs_self:
    val, tag, low = vt_entry(0x141E8, off)
    print('vt[+%#05x] = %s  tag=%s impl=%s' % (off, val, tag, low))

print()
print('=== gate/provider offsets on AB vtable ===')
for off in offs_gate + offs_prov:
    val, tag, low = vt_entry(0x141E8, off)
    print('vt[+%#05x] = %s  tag=%s impl=%s' % (off, val, tag, low))

syms = json.load(open('syms_16.json'))
sym_by_val = {}
for s in syms:
    try: v = int(s['val'])
    except Exception: continue
    sym_by_val[v] = s['name']

print()
print('=== symbol matches (AB + UC vtables) ===')
seen = set()
for base, name in [(0x141E8,'AB'), (0x14B98,'UC')]:
    for off in sorted(set(offs_self + offs_gate + offs_prov)):
        val, tag, low = vt_entry(base, off)
        if tag == '0x8011' and low not in seen:
            nm = sym_by_val.get(low, '?')
            print('%s vt[+%#05x] -> %s  %s' % (name, off, low, nm))
            seen.add(low)
