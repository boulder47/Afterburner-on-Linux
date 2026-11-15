import struct, json, subprocess
data = open('../arm64e.bin','rb').read()
BASE = 0x141E8
def decode(off):
    v = struct.unpack_from('<Q', data, BASE+off)[0]
    tag = (v >> 48) & 0xFFFF
    lo = v & 0xFFFFFFFFFFFFFFFF
    if tag == 0:
        return f"raw=0x{v:x}"
    low32 = v & 0xFFFFFFFF
    if tag in (0x8011,):
        return f"LOCAL fn @0x{low32:x}"
    elif tag in (0xc011,):
        try:
            imps = json.load(open('imports_map.json'))
            e = imps[str(low32)] if isinstance(imps, dict) else imps[low32]
            name = e.get('name') or str(e)
            d = subprocess.run(['c++filt', name], capture_output=True, text=True).stdout.strip()
            return f"IMPORT[{low32}] {d}"
        except Exception as ex:
            return f"IMPORT[{low32}] ?({ex})"
    else:
        return f"tag=0x{tag:x} val=0x{v:x}"

for i in range(48):
    off = i*8
    print(f"vf#{i:02d} +0x{off:03x}: {decode(off)}")
