# Afterburner Linux Driver — Plan v2 (streamlined, 2026-11-12)

**Goal (unchanged from plan.md):** a complete production **Linux kernel module + userspace
interface** that drives the Apple Afterburner ProRes decoder card at PCI `0f:00.0`
(`[106b:0205]`) — decode-only, per Apple's own docs (no encoder datapath exists to port).

This document replaces plan.md as the forward-looking plan of record. All historical detail
(Phases 0–3t, test-module pile, RE batches) lives in `FINDINGS.md` / `UC_METHODS.md`; this file
states only what is done, what remains, and the constraints that are non-negotiable.

## The card (verified facts — do not re-derive)

- **Decode-only** ProRes accelerator; fw 2.62 capability matrix is COMPLETE (§37/§38):
  decodes **ProRes 422 Proxy (`apco`) + Standard (`apcn`) + HQ (`apch`)** pixel-perfect vs
  FFmpeg (±1 rounding); **acks-but-zero-output = 4444 (`ap4h`) and ProRes RAW** — profile-based
  rejection, present in every shipped bitfile Dec-2019 → Sequoia 15.x (fw never updated; §37.4).
- BAR0 = 128K control/status; BAR1 = 64K command/doorbell/status surface (§21). Descriptor
  chains live in system RAM (coherent DMA); the host has **no IOMMU** → chunked coherent
  fallback for large regions (§33.2).
- Working submit path = **batch/receive channel**: stage chain-head PA at BAR1+0x5080..C, ring
  BAR1+0x1004 with `0x80FFE7F`; completion BAR1+0x2044 (bit0 valid, [31:1] count), data words
  +0x44/+0x1044; ack write-backs to +0x2018/+0x2014 are **engine-owned and ignored** (§3n).
- Output = fixed interleaved YUV422 **8-bit**, input W/H, padded layout: stride =
  ceil(W·2/64)·64 B, rows = ceil(H/16)·16 (extra rows copy last real row); chroma is
  **full-vertical, aligned to luma rows** — direct card-row y ↔ ref-row y mapping (§38.3; the
  earlier "half-vertical even-rows-only" claim in §32 was a comparison artifact and is REFUTED).
  No format/dimension field exists in the protocol.

## State (2026-11-12)

| Done | Evidence |
|---|---|
| Full static RE of kext + UC ABI (batches 1–3, all methods byte-verified) | FINDINGS §21–§30, UC_METHODS §1–§21 |
| Production module `linux-driver/afterburner/` (ab_pci / ab_regs / ab_engine / ab_dma / ab_chardev) — short probe, ioctl-triggered decode, stop-on-remove, err_handler | FINDINGS §31.1 |
| Multi-frame decode via ioctl: per-frame reset parity + cmd_word delta semantics; output verified vs FFmpeg reference (luma 100% ±1) | FINDINGS §32 |
| V-matrix **V1–V5 all PASSED**: len=64 regression, fixture chain, 4K end-to-end (+75 s idle AER watch), multi-frame both channels (count≥1 path), induced-stall watchdog + recovery | FINDINGS §33–§36 |
| Profile matrix complete; output padding rules pinned live | FINDINGS §35.3, §37 |
| **Phase A done**: sysfs surface (14 attrs) + `abctl` client validated end-to-end on hardware — single-frame & multi-frame decodes pixel-perfect ±1 vs FFmpeg ref; apcn added to decode matrix; §32 chroma claim refuted | FINDINGS §38 |

**Remaining:** Phase B (hardening) → C (V6 destructive, LAST) → D (flash port, optional).
Card is alive and the module currently loaded clean.

## Remaining phases

### ~~Phase A — Userspace completion~~ ✅ DONE 2026-11-12 (§38)

Delivered: `ab_sysfs.c` (14 live-read attributes), `userspace/abctl.c` (`status`,
`decode [--nostop]`, `start`, `stop`; decode stops the decoder by default — V6 kill-state
avoidance; devnode 0666 via class devnode callback so abctl runs unprivileged). All acceptance
criteria met and then some: fresh-insmod attrs sane (fw 2.62, ready=1, temps 26–39 °C); fixture
round-trip ±1 vs FFmpeg ref; multi-frame warm-decoder sequence with real motion all ±1;
`apcn` (ProRes Standard) confirmed decoding — matrix expanded beyond §37.

### Phase B — Hardening (NEXT, 3u.4 remainder)

- Multi-frame batching across the 8 channel blocks (BAR0+0x8000..+0xf000, stride 0x1000):
  currently one decoder instance/channel is used; kext runs N slots concurrently. Design from
  UC_METHODS §15/§16 (per-slot model) before touching hardware.
- Long idle soak with driver attached + decoder enabled (the state macOS never leaves — VT feeds
  continuously or stop() disables; our V3 watch ran with the decoder *disabled*).
- RECOVER ioctl soak: repeated wedge → re-bringup cycles, verify no drift in ack registers.

*Accept:* N consecutive frames across ≥2 channels without a wedge; watchdog + recovery still
clean after batching lands; 1 h idle soak, zero AER (dmesg-watched).

### Phase C — V6 destructive validation (LAST)

Full frame, **no stop command**, ≥75 s idle + AER watch. Expected to die (mode-15 control);
confirms the stop-command necessity that §28/§33 already imply. Run only after B; card needs a
power cycle afterward — schedule accordingly and re-run V1 as post-revive regression.

### Phase D — Flash / bitfile port (optional, opt-in)

RE complete (§20/§29.3): SPI cmd block BAR0+0x3000 (status@+0x10, addr@+0x40, cmd@+0x48,
data@+0x4C, done@+0x54), two-phase live→golden update, erase/program opcodes 0xEEEE /
(len<<16)|0xAAAA at flash addr 0x1400000. Only if we ever need to re-image the FPGA from Linux;
high risk (bricks the card on a bad write) — explicit userspace flag, read-back verify, last.

## Hard constraints (live-verified — non-negotiable)

1. **≤126 output descriptors per chain** (fw cap; 257 → deterministic MalfTLP, §28.1). Chain
   builder hard-errors above the cap; `out_seg` clamps accordingly.
2. **Never run long sequences inside `.probe`** (§28.2 — one card kill: dev->mutex held across
   probe blocked AER recovery ~80 s). All decode work is ioctl-triggered in process context with
   bounded poll windows.
3. **Engine-owned registers:** BAR1+0x2018/+0x2014 writes are ignored — never try to clear them;
   recovery = decoder re-bringup cycle (rung A, §3n) or FLR.
4. **Decoder enable persists across rmmod/insmod** → bring-up must be idempotent (ready bit may
   already be set on a warm card).
5. **BAR0+0x202C is not writable** — write-and-ignore with a comment (§3k).
6. **Liveness checks include dmesg/AER**, not just register polling (registers read "alive" even
   post-fatal-AER, §28).
7. PAs fit in 32 bits on this box (observed ≤0x47xxxxxx; kext numAddressBits=0→32) — keep ring +
   buffers below the 4 GiB PA boundary or use a 32-bit DMA mask.

## Validation matrix status

| # | test | status |
|---|---|---|
| V1 | len=64 single-shot + VALID ack (regression baseline) | ✅ §33.1 |
| V2 | fixture frame chain, batch/receive; output vs FFmpeg ref | ✅ §32.5 |
| V3 | full 4K frame, out_seg=4MB, stop cmd, ≥75 s idle + AER watch | ✅ §33.3 (p4k partial-decode at slice #3418 = content-specific fw behavior, §34) |
| V4 | multi-frame ×N; count≥1 batch completion (+0x1044 path) both channels | ✅ §36 |
| V5 | induced stall → watchdog reset/re-bringup | ✅ §36 |
| V6 | full frame, NO stop, ≥75 s idle + AER watch (destructive, LAST) | ⬜ Phase C |

## Build & layout

```
linux-driver/afterburner/
├── Makefile            # Kbuild: afterburner-y := ab_pci.o ab_regs.o ab_engine.o ab_dma.o ab_chardev.o ab_sysfs.o
├── ab_pci.c            # probe/remove/err_handler — SHORT PROBE ONLY (constraint 2)
├── ab_regs.h/.c        # named constants + readl/writel accessors
├── ab_engine.c         # bring-up / stop / recovery (kext-exact)
├── ab_dma.c            # chain builder + doorbell + completion poll/ack + per-frame reset
├── ab_chardev.c        # /dev/afterburner0 ioctls (UC ABI mirror) + mmap (UC, No-Snoop-safe)
├── ab_sysfs.c          # sysfs attributes (Phase A)
├── include/{ab.h,ab_regs.h}
├── userspace/abctl.c   # supported client (Phase A)
└── tests/              # validation-matrix clients (abtest*, abctl refs: wrap_prores.py,
                        # compare_card_ref.py — supersedes v3_compare.py's chroma model, §38.3)
```

Build: `make` in the module dir. Load: `sudo insmod afterburner.ko [out_seg=N]`.
Device node `/dev/afterburner0`; sysfs under `/sys/class/afterburner/afterburner0/`.

## Risks (open)

| risk | mitigation |
|---|---|
| AER during a long ioctl test | process context, no dev->mutex across work; err_handler logs; bounded polls; dmesg watcher for V-runs |
| 126-desc cap hit by user input | chain builder hard-errors above cap; out_seg clamped |
| wedged engine mid-test | RECOVER ioctl (re-bringup first, FLR second); power cycle documented last resort |
| decoder already enabled from prior session | idempotent bring-up (ready-bit check before enable cycle) |
| output layout mismatch in new clients | fixed interleaved yuv422p8 at input W/H with §35 padding — abctl computes it from the frame header; verify vs FFmpeg ref on every V-run |
