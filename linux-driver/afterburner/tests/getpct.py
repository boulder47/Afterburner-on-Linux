import fcntl, struct, sys
AB_IOC_GET_PERCENT = (1<<30)|(4<<16)|(ord("A")<<8)|6
fd = open('/dev/afterburner0', os.O_RDWR) if False else None
import os
fd = os.open('/dev/afterburner0', os.O_RDWR)
buf = bytearray(4)
fcntl.ioctl(fd, AB_IOC_GET_PERCENT, buf)
v = struct.unpack('<I', bytes(buf))[0]
print(f'getPercent -> {v} (raw 0x{v:08x})')
