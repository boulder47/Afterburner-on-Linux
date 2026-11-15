#!/usr/bin/env python3
"""Region before AB instance table (0x141C0-0x14200) + UC MC table local slots."""
import struct, capstone
d=open('rework/arm64e.bin','rb').read()
def q(o): return struct.unpack_from('<Q',d,o)[0]

print('=== 0x141C0-0x14200 (before AB instance table) ===')
for off in range(0x141C0, 0x14200, 8):
    v=q(off); print(f'@{off:05X}: {v:016x} tag=0x{(v>>48)&0xFFFF:04x}')

print()
print('=== UC MC table (live base 0x15588) local fn slots ===')
for i in range(32):
    off = 0x15588 + i*8
    v=q(off); tag=(v>>48)&0xFFFF; low=v&0xFFFFFFFF
    if tag==0x8011: print(f'vf#{i} @{off:05X}: fn@{low:x}')

print()
print('=== AB MC table (live base 0x14A90) local fn slots ===')
for i in range(32):
    off = 0x14A90 + i*8
    v=q(off); tag=(v>>48)&0xFFFF; low=v&0xFFFFFFFF
    if tag==0x8011: print(f'vf#{i} @{off:05X}: fn@{low:x}')

print()
print('=== AB instance table (live base 0x141F8) local fn slots ===')
for i in range(272):
    off = 0x141F8 + i*8
    v=q(off); tag=(v>>48)&0xFFFF; low=v&0xFFFFFFFF
    if tag==0x8011: print(f'vf#{i} @{off:05X}: fn@{low:x}')

print()
print('=== UC instance table (live base 0x14BA8) local fn slots ===')
for i in range(316):
    off = 0x14BA8 + i*8
    v=q(off); tag=(v>>48)&0xFFFF; low=v&0xFFFFFFFF
    if tag==0x8011 or tag==0x8041: print(f'vf#{i} @{off:05X}: fn@{low:x} tag=0x{tag:04x}')
