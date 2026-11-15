#!/usr/bin/env python3
"""Dump sMethods table @0x15680 (7 x 24B) + UC slot 314, to settle trampoline addresses."""
import struct
d = open('rework/arm64e.bin','rb').read()

def q(off): return struct.unpack_from('<Q', d, off)[0]

print('=== sMethods table @0x15680 (7 records x 24B) ===')
for i in range(7):
    base = 0x15680 + i*24
    f0 = q(base); f1 = q(base+8); f2 = q(base+16)
    print(f'rec {i} @ {base:05X}: tramp=0x{f0&0xFFFFFFFF:08x}(tag 0x{(f0>>48)&0xFFFF:04x}) '
          f'f1={f1:016x} (scalar_in={f1 & 0xFF}, in_bytes={(f1>>32)&0xFFFFFFFF if (f1>>56)==0 else "?"}) '
          f'out_qw=0x{f2&0xFFFFFFFF:08x}')

print()
print('=== UC slot 314 @ %05X ===' % (0x14B98+314*8))
v = q(0x14B98+314*8)
print(f'{v:016x} tag=0x{(v>>48)&0xFFFF:04x} mid=0x{(v>>32)&0xFFFF:04x} low=0x{v&0xFFFFFFFF:08x}')

# If slot 314 is a sMethods-trampoline tag (0x8041), low might index into sMethods
print()
print('=== UC slots 305-320 raw dump ===')
for i in range(305, 320):
    v = q(0x14B98+i*8)
    print(f'UC slot {i} @ {0x14B98+i*8:05X}: {v:016x}')

print()
print('=== strings near 0x3000-0x3200 (selector names?) ===')
seg = d[0x3000:0x3200]
i = 0
while i < len(seg):
    if seg[i:i+1].isalpha() or seg[i:i+1] == b'_':
        j = i
        while j < len(seg) and (seg[j:j+1].isalnum() or seg[j:j+1] in (b'_.-')):
            j += 1
        s = seg[i:j]
        if len(s) >= 4:
            print(f'@{0x3000+i:05X}: {s!r}')
        i = j
    else:
        i += 1
