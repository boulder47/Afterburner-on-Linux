import struct
data = open('../arm64e.bin','rb').read()
ncmds = struct.unpack_from('<I', data, 16)[0]
off = 32
for i in range(ncmds):
    cmd, csize = struct.unpack_from('<II', data, off)
    if cmd == 0x19:
        segname = data[off+8:off+24].split(b'\x00')[0].decode()
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, off+24)
        nsects = struct.unpack_from('<I', data, off+64)[0]
        print('SEG %-14s @%#06x vm=%#x/%#x sect=%d' % (segname, off, vmaddr, vmsize, nsects))
        so = off + 72
        for s in range(nsects):
            secname = data[so:so+16].split(b'\x00')[0].decode()
            addr, size, offset, align, reloff, nreloc = struct.unpack_from('<QQIIII', data, so+32)
            print('   [%#06x] %-24s vm=%#x/%#x fileoff=%#x' % (so, secname, addr, size, offset))
            so += 80
    off += csize
