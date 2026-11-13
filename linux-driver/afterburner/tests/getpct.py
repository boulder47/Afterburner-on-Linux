#!/usr/bin/env python3
"""getpct.py — read getPercent (decode duty cycle since last call).

ioctl encoding must match ab_chardev.c exactly: _IOR('A', 6, u32) =
(2<<30)|(4<<16)|('A'<<8)|6 = 0x80044106.  (dir=READ is 2, not 1 — an
earlier draft used 1<<30 and got a spurious ENOTTY.)
"""
import fcntl, os, struct

AB_IOC_GET_PERCENT = (2 << 30) | (4 << 16) | (ord("A") << 8) | 6
assert AB_IOC_GET_PERCENT == 0x80044106

fd = os.open("/dev/afterburner0", os.O_RDWR)
buf = bytearray(4)
fcntl.ioctl(fd, AB_IOC_GET_PERCENT, buf)
v = struct.unpack("<I", bytes(buf))[0]
print(f"getPercent -> {v} (raw 0x{v:08x})")
