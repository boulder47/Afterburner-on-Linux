import struct, json, subprocess, sys
data = open('../arm64e.bin','rb').read()
st = json.load(open('strtab_1862c.json'))
names = st if isinstance(st, list) else [v for v in st.values()]

filt_cache = {}
def demangle(raw):
    if raw not in filt_cache:
        out = subprocess.run(['c++filt', raw], capture_output=True, text=True).stdout.strip()
        filt_cache[raw] = out
    return filt_cache[raw]

base = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x141E8
start, end = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x140, (int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x700)
for off in range(start, end, 8):
    val = struct.unpack_from('<Q', data, base + off)[0]
    tag = (val >> 48) & 0xFFFF
    low32 = val & 0xFFFFFFFF
    if tag == 0xc011:
        i = low32
        raw = names[i] if i < len(names) else '?'
        print('+%#05x  import %3d  %s' % (off, i, demangle(raw)))
    elif tag == 0x8011:
        print('+%#05x  LOCAL   %#x  <-- OVERRIDDEN' % (off, low32))
    else:
        print('+%#05x  tag=%#04x val=%s' % (off, tag, hex(val)))
