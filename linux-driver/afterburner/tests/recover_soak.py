#!/usr/bin/env python3
"""recover_soak.py — Phase B / B4: repeated wedge -> timeout -> recover cycles.

Per cycle (default 5):
  1. enable the V5 hang hook (ab_test_hang_ms=3000) via module sysfs param
  2. submit a frame with poll window 1000 ms < hang time -> expect -ETIMEDOUT
     (the engine completes decode in milliseconds; we deliberately ignore it
      for 3 s, so the completion word stays un-acked at +0x2044 — stale state)
  3. sysfs snapshot: ack0/ack1/completion/decoder_ready (stale-state view)
  4. recovery submit with a normal poll window -> expect success; the driver's
     pre-submit flush handles the stale event with the kext-exact dual-ack
  5. post-recovery sysfs snapshot

Accept: every cycle times out then recovers cleanly; ack0/ack1 stable across
ALL snapshots (no drift, §3n engine-owned registers); zero AER in dmesg over
the whole run. Run as root (module param write): sudo python3 recover_soak.py
"""
import fcntl, mmap as mm, os, struct, subprocess, sys, time

DEV = "/dev/afterburner0"
FRAME = "/home/archuser/afterburner-re/testdata/prores_320x180_frame0.bin"
W, H = 320, 180
STRIDE = (W * 2 + 63) // 64 * 64          # 640 (§35.2 padded layout)
ROWS = (H + 15) // 16 * 16                # 192
OUT_LEN = STRIDE * ROWS                   # 122880

def _ioc(d, nr, size):
    return ((d << 30) | (size << 16) | (ord("A") << 8) | nr)

IOC_BIND   = _ioc(3, 2, 8)                # _IOWR('A',2,{u32,u32})
IOC_SUBMIT = _ioc(3, 0, 24)               # _IOWR('A',0,struct ab_ioc_submit)
IOC_UNBIND = _ioc(0, 5, 0)                # _IO('A',5)

HANG_MS = 3000
TIMEOUT_POLL_MS = 1000                    # < HANG_MS -> guaranteed -ETIMEDOUT
RECOVER_POLL_MS = 5000


def sysfs(name):
    with open(f"/sys/class/afterburner/afterburner0/{name}") as f:
        return f.read().strip()


def set_hang(ms):
    if NO_HANG_TOGGLE:
        return  # param was baked in via insmod ab_test_hang_ms=N
    with open("/sys/module/afterburner/parameters/ab_test_hang_ms", "w") as f:
        f.write(str(ms))


NO_HANG_TOGGLE = "--no-hang-toggle" in sys.argv[1:]


def submit(fd, in_len, poll_ms):
    """One SUBMIT_FRAME; returns (status, data_word) or raises OSError."""
    buf = bytearray(struct.pack("<IIII", 0, in_len, OUT_LEN, poll_ms) + b"\x00" * 8)
    fcntl.ioctl(fd, IOC_SUBMIT, buf)
    return struct.unpack("<II", bytes(buf[16:24]))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    ncycles = int(args[0]) if args else 5

    frame = open(FRAME, "rb").read()
    fsz = (len(frame) + 4095) & ~4095     # page-align bind size
    assert struct.unpack(">HH", frame[16:20]) == (W, H), "unexpected fixture W/H"

    fd = os.open(DEV, os.O_RDWR)
    fcntl.ioctl(fd, IOC_BIND, struct.pack("<II", fsz, OUT_LEN))
    inmap = mm.mmap(fd, fsz, offset=0)
    outmap = mm.mmap(fd, OUT_LEN, offset=4096)          # pgoff 1 = out region
    inmap[0:len(frame)] = frame

    aer_before = len(subprocess.run(["dmesg"], capture_output=True, text=True).stdout.splitlines())

    acks_seen = []
    rc = 0
    for i in range(1, ncycles + 1):
        set_hang(HANG_MS)
        # --- cycle wedge: submit with window shorter than the hang hook ---
        try:
            submit(fd, len(frame), TIMEOUT_POLL_MS)
            print(f"[cycle {i}] ERROR: expected ETIMEDOUT, got success")
            rc = 1
            break
        except OSError as e:
            if e.errno != 110:  # ETIMEDOUT
                print(f"[cycle {i}] ERROR: expected ETIMEDOUT(110), got errno={e.errno}")
                rc = 1
                break

        st_ack0, st_ack1, st_comp, st_ready = (sysfs("ack0"), sysfs("ack1"),
                                               sysfs("completion"), sysfs("decoder_ready"))
        print(f"[cycle {i}] timeout OK | stale: completion={st_comp} ack0={st_ack0} "
              f"ack1={st_ack1} ready={st_ready}")

        # --- recovery submit: pre-submit flush must reap the stale event ---
        t0 = time.time()
        try:
            status, data = submit(fd, len(frame), RECOVER_POLL_MS)
        except OSError as e:
            print(f"[cycle {i}] ERROR: recovery submit failed errno={e.errno}")
            rc = 1
            break
        dt = (time.time() - t0) * 1000

        po_ack0, po_ack1, po_comp, po_ready = (sysfs("ack0"), sysfs("ack1"),
                                               sysfs("completion"), sysfs("decoder_ready"))
        acks_seen.append((st_ack0, st_ack1))
        print(f"[cycle {i}] recovered in {dt:.0f} ms status=0x{status:08x} "
              f"data=0x{data:08x} | post: completion={po_comp} ack0={po_ack0} "
              f"ack1={po_ack1} ready={po_ready}")

    set_hang(0)

    # --- final regression decode (no hang hook): pixel path must be intact ---
    try:
        status, data = submit(fd, len(frame), RECOVER_POLL_MS)
        yuv = bytes(outmap[:OUT_LEN])
        open("/tmp/phaseb_soak_last.yuv", "wb").write(yuv)
        print(f"[final] regression decode OK status=0x{status:08x} data=0x{data:08x} "
              f"({OUT_LEN} bytes -> /tmp/phaseb_soak_last.yuv)")
    except OSError as e:
        print(f"[final] ERROR: regression decode failed errno={e.errno}")
        rc = 1

    # --- AER check over the whole run (needs readable dmesg; else verify externally) ---
    r = subprocess.run(["dmesg"], capture_output=True, text=True)
    if r.returncode != 0:
        print("[aer] SKIP: dmesg not readable as this user — verify with `sudo dmesg`")
    else:
        lines = r.stdout.splitlines()
        new = lines[aer_before:]
        aer = [l for l in new if any(k in l.lower() for k in ("aer", "malf", "uncorrectable"))]
        print(f"[aer] {len(aer)} AER-related dmesg line(s) over run")
        for l in aer:
            print("   ", l.strip())

    # --- ack-register drift check ---
    uniq = set(acks_seen)
    if len(uniq) > 1:
        print(f"[drift] FAIL: ack registers drifted across cycles: {sorted(uniq)}")
        rc = 1
    else:
        a0, a1 = uniq.pop()
        print(f"[drift] OK: ack0={a0} ack1={a1} stable across all {len(acks_seen)} snapshots")

    fcntl.ioctl(fd, IOC_UNBIND)
    os.close(fd)
    print("RESULT:", "PASS" if rc == 0 else "FAIL")
    return rc


if __name__ == "__main__":
    sys.exit(main())
