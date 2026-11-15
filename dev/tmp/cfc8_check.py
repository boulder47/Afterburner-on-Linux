#!/usr/bin/env python3
"""Verify: is fn@0xcfc8 in any of the three vtables? Check UC slot 307 specifically."""
import struct

d = open('rework/arm64e.bin','rb').read()

def q(off): return struct.unpack_from('<Q', d, off)[0]

# Three vtables (batch-3 corrected boundaries)
vts = {
    'AB inst':  (0x141F8, 271),
    'metaclass obj': (0x14A90, 29),
    'UC inst':  (0x14B98, 320),
}

print('=== UC slot 307 (claimed externalMethod override) ===')
v = q(0x14B98 + 307*8)
tag = (v >> 48) & 0xFFFF
mid = (v >> 32) & 0xFFFF
low = v & 0xFFFFFFFF
print(f'@{0x14B98+307*8:05X}: {v:016x}  tag=0x{tag:04x} mid=0x{mid:04x} low=0x{low:08x}')

print()
print('=== scan all three vtables for low32 == 0xcfc8 (any tag) ===')
found = False
for name,(base,n) in vts.items():
    for i in range(n):
        v = q(base + i*8)
        if (v & 0xFFFFFFFF) == 0xCFC8:
            print(f'{name} slot {i} @ {base+i*8:05X}: {v:016x}')
            found = True
if not found:
    print('NOT FOUND in any vtable')

print()
print('=== scan ALL of 0x14000-0x15700 for low32 == 0xcfc8 ===')
for off in range(0x14000, 0x15700, 8):
    v = q(off)
    if (v & 0xFFFFFFFF) == 0xCFC8:
        print(f'@{off:05X}: {v:016x}')

print()
print('=== what local-fn slots exist in UC vtable (tag 0x8011)? ===')
for i in range(320):
    v = q(0x14B98 + i*8)
    tag = (v >> 48) & 0xFFFF
    if tag == 0x8011:
        print(f'UC slot {i} @ {0x14B98+i*8:05X}: low=0x{v&0xFFFFFFFF:08x}')

print()
print('=== what local-fn slots exist in AB vtable (tag 0x8011)? ===')
for i in range(271):
    v = q(0x141F8 + i*8)
    tag = (v >> 48) & 0xFFFF
    if tag == 0x8011:
        print(f'AB slot {i} @ {0x141F8+i*8:05X}: low=0x{v&0xFFFFFFFF:08x}')

print()
print('=== metaclass obj vtable (29 slots) full dump ===')
for i in range(29):
    v = q(0x14A90 + i*8)
    tag = (v >> 48) & 0xFFFF
    print(f'MC slot {i} @ {0x14A90+i*8:05X}: {v:016x} tag=0x{tag:04x}')
