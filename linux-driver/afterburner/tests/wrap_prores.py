#!/usr/bin/env python3
"""wrap_prores.py — wrap a raw ProRes sample ([u32BE size]["icpf"]...) in a
minimal single-frame MOV so FFmpeg can decode it to a reference YUV.

Structure mirrors a real ffmpeg-muxed ProRes mov (verified against
tmp/p4k_f0.mov): minf = vmhd + hdlr + dinf/dref/url(self) + stbl, with the
sample entry carrying data_reference_index=1 and no extra codec boxes.

Usage: wrap_prores.py <sample.bin> <out.mov> [fourcc]   (default fourcc apco)
"""
import struct, sys


def atom(typ, *payloads):
    body = b"".join(payloads)
    return struct.pack(">I", 8 + len(body)) + typ + body


IDENTITY = (0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000)


def mvhd(timescale=600, duration=600):
    body = struct.pack(">B3sIIII", 0, b"\x00\x00\x01", 0, 0, timescale, duration)
    body += struct.pack(">IHH8s", 0x00010000, 0x0100, 0, b"\x00" * 8)
    body += struct.pack(9 * "I", *IDENTITY)
    body += struct.pack(6 * "I", *(0,) * 6) + struct.pack(">I", 1)  # predefined, next_track_id
    return atom(b"mvhd", body)


def tkhd(track_id=1, duration=600, w=320, h=180):
    body = struct.pack(">B3sIIIII", 0, b"\x00\x00\x07", 0, 0, track_id, 0, duration)
    body += struct.pack(">8sHHHH", b"\x00" * 8, 0, 0, 0, 0)  # reserved, layer, alt, volume, resv
    body += struct.pack(9 * "I", *IDENTITY)
    body += struct.pack(">II", w << 16, h << 16)
    return atom(b"tkhd", body)


def mdhd(timescale=600, duration=600):
    body = struct.pack(">B3sIIII", 0, b"\x00\x00\x00", 0, 0, timescale, duration)
    body += struct.pack(">HH", 0x55C4, 0)  # 'und', predefined
    return atom(b"mdhd", body)


def hdlr(handler=b"vide"):
    body = struct.pack(">I4s12s", 0, handler, b"\x00" * 12) + b"\x00"  # name (null-terminated)
    return atom(b"hdlr", body)


def vmhd():
    body = struct.pack(">B3sH", 0, b"\x00\x00\x01", 0)  # flags=enabled, graphicsmode
    body += struct.pack(3 * "H", 0, 0, 0)               # opcolor
    return atom(b"vmhd", body)


def dinf():
    url = atom(b"url ", struct.pack(">B3s", 0, b"\x00\x00\x01"))  # self-contained flag=1
    dref = atom(b"dref", struct.pack(">II", 0, 1), url)  # FullBox: version/flags + count
    return atom(b"dinf", dref)


def stsd(fourcc, w, h):
    body = struct.pack(">6sH", b"\x00" * 6, 1)            # reserved + data_reference_index=1
    body += struct.pack(">HH12s", 0, 0, b"\x00" * 12)     # pre_defined, reserved, pre_defined[3]
    body += struct.pack(">HHII", w, h, 0x00480000, 0x00480000)
    body += struct.pack(">IH", 0, 1)                      # reserved(4), frame_count
    body += b"\x00" * 32                                  # compressorname
    body += struct.pack(">Hh", 0x0018, -1)                # depth=24, pre_defined=-1
    entry = atom(fourcc.encode(), body)
    return atom(b"stsd", struct.pack(">II", 0, 1), entry)  # FullBox: version/flags + count


def stts(n=1, delta=600):
    return atom(b"stts", struct.pack(">IIII", 0, 1, n, delta))


def stsc():
    return atom(b"stsc", struct.pack(">IIIII", 0, 1, 1, 1, 1))


def stsz(size, n=1):
    return atom(b"stsz", struct.pack(">III", 0, size, n))


def main():
    sample_path, out_path = sys.argv[1], sys.argv[2]
    fourcc = sys.argv[3] if len(sys.argv) > 3 else "apco"
    sample = open(sample_path, "rb").read()
    w = int.from_bytes(sample[16:18], "big")
    h = int.from_bytes(sample[18:20], "big")

    ftyp = atom(b"ftyp", b"isom" + struct.pack(">I", 512) + b"isomiso2")
    stco_placeholder = atom(b"stco", struct.pack(">III", 0, 1, 0))  # FullBox: v/f + count + off=0
    stbl = atom(b"stbl", stsd(fourcc, w, h), stts(), stsc(), stsz(len(sample)), stco_placeholder)
    minf = atom(b"minf", vmhd(), hdlr(), dinf(), stbl)
    mdia = atom(b"mdia", mdhd(), hdlr(), minf)
    trak = atom(b"trak", tkhd(w=w, h=h), mdia)
    moov = atom(b"moov", mvhd(), trak)

    mdat_payload_off = len(ftyp) + len(moov) + 8
    marker = b"stco" + struct.pack(">III", 0, 1, 0)
    idx = moov.find(marker)
    assert idx >= 0, "stco placeholder not found"
    moov = moov[:idx] + b"stco" + struct.pack(">III", 0, 1, mdat_payload_off) + moov[idx + len(marker):]

    mdat = atom(b"mdat", sample)
    open(out_path, "wb").write(ftyp + moov + mdat)
    print(f"wrote {out_path}: {w}x{h}, fourcc={fourcc}, sample={len(sample)} B")


if __name__ == "__main__":
    main()
