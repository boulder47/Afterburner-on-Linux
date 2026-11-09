# Linux Driver Plan — Apple "Afterburner" FPGA Card `[106b:0205]`

Goal: a working **Linux kernel module** (PCI driver) + userspace interface that drives the
Apple Afterburner card installed at PCI `0f:00.0`, reusing the hardware contract recovered in
`FINDINGS.md`. The macOS kext (`com.apple.driver.AppleAfterburner`) does not run on Linux; this
plan ports only the OS-independent register/DMA/flash protocol and rebuilds everything that was
IOKit from scratch with Linux primitives.

## What the card actually is (Apple official, support.apple.com/en-us/101662)

> "Apple Afterburner is an accelerator card for Mac Pro (2019), created to enhance Apple ProRes
> and ProRes RAW workflows… **Afterburner accelerates decoding and playback** of multiple streams
> of ProRes and Pro Res RAW video files."

- **Decode-only.** Apple's FAQ is explicit: *"Does Afterburner accelerate ProRes or ProRes RAW
  encoding? — Afterburner accelerates ProRes and Pro Res RAW **decoding and playback**."* It does
  **not** encode. (The user's initial framing of "decode/encode" is corrected by Apple's own docs:
  decode + playback only.)
- **All ProRes + ProRes RAW codecs**, except interlaced ProRes media.
- **Multi-stream**: a 28-core Mac Pro + Afterburner plays back ~6× 8K DCI ProRes RAW or ~23× 4K
  ProRes RAW at 29.97 fps in full quality (Final Cut Pro). Up to **3 cards** stack for more decode.
- **ProRes RAW demosaicing is done by the GPU**, not the card — the card hands decoded frames out;
  a downstream consumer (GPU/CPU) finishes RAW→RGB.
- Works with Final Cut Pro, Motion, Compressor, QuickTime Player (any app that plays ProRes/RAW).
- macOS-only in practice: *"decoding acceleration is available in macOS and is not available when
  using Windows with Boot Camp."* Our Linux port is the first non-macOS attempt at this hardware.

**Implication for our driver:** "success" = **decode a ProRes (or ProRes RAW) stream to raw frames**,
not encode. The DMA path we're building (Phase 3) is *encoded-in → decoded-out*: compressed ProRes
frames go in via the descriptor ring, decoded YUV/RGB frames come out. There is no encoder datapath
to port — that simplifies Phase 3 and means we should not look for encode registers.

## Status (live)

| Phase | State | Notes |
|---|---|---|
| 0 — Bring-up & discovery | ✅ **DONE** | `afterburner_map.ko` captured full BAR0 map (`linux-driver/bar_map.txt`). Card alive, reads cleanly with no init. |
| 1 — Register naming / accessor layer | ✅ **DONE** | `afterburner_full.ko` dumped entire live region incl. zeros (`full_dump.txt`). Every family named to a verified BAR0 offset via section-header magics — see FINDINGS.md §13. |
| 2a — First safe write-test + IRQ discovery | ✅ **DONE** | `afterburner_flash{,2}.ko`: non-destructive Flash READ passed (writes are safe), MSI confirmed allocatable (1×64-bit vec). Live register space = `+0x8000..+0xf000` (7 engine banks); rest unmapped. Flash cmd block location deferred to Phase 5 — see FINDINGS.md §14. |
| 2b — Bitfile interrupt handler | ✅ **DONE** | `afterburner_irq.ko`: MSI enabled (vector 334, msi_bus=1), §7.6 handler registered cleanly + ack protocol implemented. Self-test triggers didn't fire IRQ (expected — needs a real decode event). Trigger validation deferred to Phase 3; polling is the fallback. See FINDINGS.md §15. |
| 3a — DMA ring geometry | ✅ **DONE** | `afterburner_dma.ko` (read-only): resolved the ring ambiguity. Doorbell regs at `bar0+0x1004` (single) / `bar0+0x2004` (batch); descriptor ring is in SYSTEM RAM (coherent DMA), NOT the BAR. The `+0x4000` uniform array is not the ring. See FINDINGS.md §16. |
| 3b — First real submit | ✅ **DONE** | `afterburner_submit.ko`: FIRST REAL DOORBELL. Card accepted it, consumed it (cleared to 0), and latched state: counter slot 0 ctrl 0→1, engine bank 0 ctrl 0→4. Pipeline is ALIVE. desc_lo write ignored (wrong offset — open item). No IRQ (expected; polling works). Card healthy post-submit. See FINDINGS.md §17. |
| 3c — Real frame + desc-reg hunt | ✅ **DONE** (partial) | `afterburner_frame.ko`: swept 24 candidate desc-PA offsets — NONE accept a PA write. The ring location is registered at INIT time, not per-submit. 8 MB coherent alloc failed (pool too small). See FINDINGS.md §18. |
| 3d — Replicate kext init sequence | ✅ **DONE** (major correction) | Disassembly revealed: card has TWO BARs; we only mapped BAR0. BAR1 (64K @ `74c20000`) is the REAL DMA register space. Kext uses IODMACommand (IOKit DMA framework), not raw writes. See FINDINGS.md §19. |
| 3e — Pin real BAR1 doorbell + desc-PA offsets | ✅ **DONE** (superseded) | ~~Full 64KB BAR1 sweep found: desc-ring PA at BAR1+0x3080/0x3084~~ — **REFUTED by §21.6(c): no write to +0x3080 exists anywhere in kext `__text`**; the real staging banks are +0x4080..C / +0x5080..C (verified at two code sites). Doorbell-at-+0x2004 was actually the final *engine-enable* write of the init sequence, not a doorbell. See FINDINGS.md §19 Finding 6 + §21. |
| 3f — First real DMA submit | ✅ **DONE** (negative result) | `afterburner_dma_test.ko`: bare PA + doorbell does NOT trigger DMA. Card requires full IODMACommand setup (valid descriptors, channel/engine config). See FINDINGS.md §19 Finding 7. |
| 3g — Deep reverse of descriptor construction | ✅ **DONE** (negative result) | `afterburner_init_seq.ko`: replicated kext's visible init sequence (interrupt setup + desc pattern + doorbell), but still no DMA. The actual register writes happen inside IODMACommand framework, which we don't have disassembly for. See FINDINGS.md §19 Finding 8. |
| 3h — Corrected-protocol DMA submit (was: empirical hunt) | ✅ **RE complete** (§21 + §21.6) | The arm64e decode ended the register hunt: doorbells are `BAR1+0x4` (**send**, magic `0xFFE7F`) / `BAR1+0x1004` (**receive**, `0x80FFE7F`), descriptor PA staged at fixed banks `BAR1+0x4080..C` (send) / `+0x5080..C` (receive), completion status `BAR1+0x2044` acked via write-back to `+0x2018`. §21.6 adds: full init/reset decode (probe-time BAR1 words 0xFFFFFE/0xFFFFFE/3 → 0x16 + BAR0 decoder config), "Wake" pre-submit call resolved (conditional, log template VA 0x2326), +0x3080 refuted. This supersedes the 3e/3f "findings". |
| 3i — Live test of verified protocol | ✅ **DONE** (protocol VERIFIED live) | `afterburner_submit2.ko` mode=1: engine consumed the descriptor (`+0x2010` 3→2), completion `status(+0x2044)=0x1 VALID`, `data(+0x44)=6` (bit0 clear = normal path per handler @0x639c), dual ack cleared status. Re-trace of chain-builder @0x9f9c-0xa164 found the one wrong field: **desc[+0x04] must be segment length, not PA-lo** — fixed in rewrite. |
| 3j — PA-slot question settled | ✅ **DONE** (decisive) | mode=3 dual-variant run: A (PA@+0x08, kext-exact) and B (PA@+0x10) completed IDENTICALLY → engine ignores the PA slot for single-shot submits; the length field at +0x04 was the only real fix. |
| 3k — Decoder bring-up (enable + ready) | ✅ **DONE** (live, mode=4/5 run) | Bring-up ran clean: fw gate PASS (0x0002003E), walk found decoder header at BAR0+0x8000 (**stride is 0x2C, not 0 — earlier miscalc; walk well-formed, terminator word=0 at +0x802C**), disable/enable of Decoder Control Register (+0x8004) ok, init writer idempotent (magic region no-ops; **+0x202C is NOT writable** — wrote 0, readback stays 7). Ready bit BAR0+0x8008 = 0x3E0FD001 → **READY immediately** (enable sets it; persists across rmmod/insmod — card was already enabled from the mode=4 run when mode=5 ran). Submit with decoder READY: completion VALID, **data(+0x44) still 6**, wordB(+0x48)=1, BAR0+0x8014 unchanged (4→4) → a 64-byte pattern payload is not being decoded; the engine accepts and acks it regardless. |
| 3l — Real ProRes frame submit | 🔴 **Live: TIMEOUT** (frame loaded, engine did not consume) | Frame format decoded from FFmpeg `proresdec.c` (`decode_frame_header`/`decode_picture_header`): raw QuickTime sample = `[u32 BE total size]["icpf" @+4][frame hdr: u16BE len=148, ver, "fmpg", width@+8, height@+10, …][picture hdr (buf[0]>>3)][slices]`. Verified against our extracted sample: 20138 B, size field self-consistent, fmpg box, **320×180 progressive YUV422P10 no-alpha** — exactly what VideoToolbox hands the kext. mode=6 = bring-up + ready poll + **kext-exact pre-submit write @0xa714 (byte-verified: `mov w9,#0x1f; str w9,[x8,#0x18]`, x8=self->+0x158=decoder header → BAR0[hdr+0x18]=0x1F)** + single-descriptor submit of the real frame (desc[+4]=20138, PA@+0x08). Payload fixture: `testdata/prores_320x180_frame0.bin` (sha256 e41a0577…8770d). New params: `frame_path=<path>`, `poll_ms=` (default 50; use 2000 for real frames). **LIVE RUNS:** (1) wrong param name (`frame=`) → silent pattern fallback, len=64, data(+0x44)=6 as always; enable cycle itself moved BAR0+0x8024 0x201→0x1201 (fw-managed). (2) correct `frame_path=`: frame loaded (20138 B), pre-submit write landed (+0x8018=0x1F), staged + rung — **but engine NEVER consumed the descriptor: BAR1+0x2010 stayed 3, no completion in 2 s.** Same magic/staging/doorbell as every successful len=64 run; only desc[+4] (64→20138) and data PA differ. NOTE: param is `frame_path=` not `frame=` — first run used the wrong name and silently fell back to the pattern. |
| 3m — Single-shot length ceiling | 🔴 **Live: NOT a size issue** (all sizes rejected, incl. len=64) | mode=7 = BAR1-only init + sweep of the same descriptor at len = 64, 128, 256, 512, 1K, 2K, 4K, 8K, 16K, 20138; logs CONSUMED/timeout per size (status bit0) + eng-ctrl (+0x2010) before/after. **LIVE RESULT: every single size timed out — including len=64, the exact config that was consumed instantly in Phase 3i and modes 5/6-run#1.** So there is no length ceiling to find; the engine stopped accepting ANY submission sometime after mode=6 run #2 (the un-consumed 20138-byte submit). PRE-STATE anomalies vs working runs: `+0x2018 completion ack = 0x3` (Phase 3i's status was 0x1; source of the 3 unknown — possibly engine-written during the failed 20KB transfer) and stale mode=6 staging still in +0x4080..C. Hypotheses: (a) the 20KB single-shot transfer wedged the DMA channel internally, (b) non-zero ack register blocks new submits. |
| 3n — Recovery matrix | ✅ **DONE** (live: rung A works) | mode=8 = full bring-up (fw gate + walk + enable/disable/enable + init writer + ready poll), then a ladder of len=64 submits until one is consumed: **A** as-is after bring-up → **B** after `writel(0, BAR1+0x2018)` → **C** after also clearing `+0x2014` → **D** after PCIe FLR (`pci_reset_function`) + BAR1 re-init. **LIVE RESULT: rung A CONSUMED immediately — the decoder disable/enable cycle (full bring-up) alone un-wedges the engine; no ack clears, no FLR needed.** B/C never ran (short-circuit). Anomaly: writes to `+0x2018`/`+0x2014` do NOT stick (wrote 0x1, read back 0x3) — those registers are engine-owned; the only reliable reset is the decoder re-enable cycle. |
| 3o — Chain-submit ladder (multi-segment frames) | ✅ **DONE** (live: R9-1 SUCCESS, send channel dead) | mode=9/10/11 live runs with the real ProRes frame. **R9-1 (chain → batch bank +0x5080, doorbell 0x80FFE7F): engine consumed the full 5-desc chain and completed** (status VALID, count=0, data(+0x44)=6, wordB=5). mode=10 (chain → send bank +0x4080, 0xFFE7F) and mode=11 (N=5/4K + N=10/2K via send doorbell): **no completion in 30 s each**. Conclusion: batch/receive channel is the working submit path — likely kext-exact; send channel needs different setup or serves another purpose. See FINDINGS.md §21.10. |
| 3p — Static RE of kext binary (vtables/class map) | ✅ **DONE** (core), open items listed | Four vtables mapped + fully named-decoded (`rework/static_re/vtable_named.txt`); probe()=fn@0xc998, start()=fn@0x4254, externalMethod=fn@0xcfc8; import mechanism = LC_DYSYMTAB @0x520 + 368-name strtab @0x1862c (NO chained fixups); symtabs parsed (arm64e 510 / x86_64 503, 16-B nlist); sources = Athena.cpp + AthenaUserClient.cpp. Open: u32 array @0x1806c purpose, MetaClass vtable reconciliation, dispatch table @0x15680 named list, x86_64 NULLs/−2 slot shift. See FINDINGS.md §22. |
| 3q — Confirm kext frame-commit path (static) | ✅ **DONE** (2026-11-02, FINDINGS.md §25) | Premise was wrong: vtable[+0x150] is IOKit return-to-pool. Real commit = UC trampoline @0xd708 → submitFrame @0xa9ac → submitData @0xa67c; doorbell fn@0x6800 byte-verified: channel cmd[+0x1c] (userland scalar via `mov x4,x21` @0xdb14), send = clear +4 / stage +0x4080..C / ring 0xFFE7F, batch = clear +0x1004 / stage +0x5080..C / ring 0x80FFE7F — **byte-exact match with our Linux driver**. BAR1 base = ctx->+0x130 (NOT +0x148 = BAR0+0x2000). Completion handler fn@0x985c: gated by +0x12f8, polls *(ctx->+0x90) vtable[+0x200/+0x210], send-completion writes BAR0[hdr+0xC]. |
| 3r/3s — UC ABI decode + stop command (static) | ✅ **DONE** | sMethods dispatch table, trampoline ABI, submitFrame slot model; `stop()` = AB vtable slot 191, only HW write is decoder disable. See FINDINGS.md §26 + UC_METHODS.md §18/§21. |
| 3t — Static RE batch 3 (ADRPs, metaclasses, resourceLoadCallback) | ✅ **DONE** (`29e03b0`) | ADRP encoding resolved via Keystone ground truth; three vtables corrected; installResource path decoded. See FINDINGS.md §30. |
| 3u — Driver rewrite from decoded protocol (production module `linux-driver/afterburner/`) | 🟡 **IN PROGRESS** | **3u.1 ✅ PASSED** (insmod/rmmod ×5 clean, FINDINGS §31.1). **3u.2 🟡**: submit path via ioctl WORKS — 4× identical fixture frames decode correctly vs FFmpeg reference (luma 100% within ±1; chroma half-vertical, even rows only); root cause of multi-frame garbage = missing per-frame reset + cmd_word delta semantics (FINDINGS §32). V-matrix (§8.5) pending: V3/V4-rest/V5/V6. |
| 4 — Userspace interface | ⬜ | |
| 5 — Flash / bitfile mgmt | 🟡 **RE complete** (port pending) | Kext-side path fully decoded: descriptor chain, version selection by fuse word, two-phase update, SPI erase/program at flash addr `0x1400000`, pointer tables. See FINDINGS.md §20. Linux port still high-risk / last. |

> **Key correction from Phase 0/1:** the RE's seven "families" are *views into one BAR0 region*
> (`+0x1000..+0x4fff`), not separate BARs. The layout is self-describing via section-header words
> `0x040001xx` (type 2=ID, 3=decoder, 4=counters, 7=channel control) plus the DMA ring at
> `+0x4000`. ~~BAR1 is empty~~ — **superseded (§19/§21):** BAR1 (64K) is the runtime
> command/doorbell/status surface for DMA submits; it only reads empty at cold boot. One ioremap
> of BAR0 covers the register families, but a working DMA path also needs BAR1 mapped — see
> FINDINGS.md §12–§13 (BAR0 map) and §19/§21 (BAR1 runtime protocol).
---

## 1. Confirmed hardware (from `lspci` on this box)

| Field | Value |
|---|---|
| BDF | `0f:00.0` |
| Vendor : Device | **`106b : 0205`** (Apple Inc.) |
| Subsystem | `106b : 0205` |
| Class | `0480` — Multimedia controller |
| Upstream path | behind PLX PEX8796 PCIe switch (`0c:xx`) → Sky Lake-E root port |
| BAR0 | **128 KiB**, 32-bit, non-prefetchable @ `0x74c00000` — register space (the region we RE'd) |
| BAR1 | **64 KiB**, 32-bit, non-prefetchable @ `0x74c20000` — secondary MMIO (DMA/descriptor staging?) |
| IRQ | INTx (legacy), line not yet assigned (`irq=0`) because no driver has claimed it; **no MSI/MSI-X capability visible** ("Capabilities: <access denied>" until we own the device) |
| Current state | BARs `[disabled]`, `Mem- BusMaster-` — nothing has enabled the device. Our module's job is to do that. |

> Note: BAR sizes (128K / 64K) are small and match a register+staging layout, not a framebuffer.
> The DMA descriptor ring lives in **system RAM** (coherent DMA memory), not in the BARs — the
> BARs hold only control/status registers plus possibly a small staging window.

### What we still must confirm at runtime (first bring-up)
- Exact IRQ vector + whether MSI is actually available once `pci_enable_device` runs.
- Which BAR holds which RE'd region: our FINDINGS.md offsets were relative to the *driver object's*
  region pointers, not raw BAR offsets. We need a **BAR0/BAR1 → region map** (see Phase 2).
- Whether BAR1 is the DMA descriptor staging window or a second register bank.

---

## 2. What ports directly vs. what we rewrite

| Concern | Source in kext | Linux replacement | Portability |
|---|---|---|---|
| PCI match | IOKit property-match plist | `pci_driver.id_table` = `PCI_DEVICE(0x106b, 0x0205)` | trivial |
| BAR map | `IOPCIBar` in `start()` | `pci_enable_device`, `pci_request_regions`, `pci_iomap`/`ioremap` per BAR | direct |
| Region offsets (BAR0/DMA/Decode/Flash/Bitfile/Thermal/Version) | fixed offsets from FINDINGS.md §5–9 | offsets into ioremapped BAR(s) | **direct — copy the constants** |
| Register accessors `read/write{BAR0,DMA,Decode,Flash,Bitfile,Thermal,Version}Register` | MMIO via IOPCIBar | `readl()` / `writel()` on ioremap'd base | **1:1 portable** |
| DMA descriptor ring (`getDescriptorList`, 32-byte descs, tag `0xAD4B`) | `IODMACommand` + IOBufferMemoryDescriptor | `dma_alloc_coherent` (ring) + `dma_map_single`/S-G (frames); build same 32-byte descriptors | rewrite logic, reuse descriptor layout |
| Doorbells (`submitDataGated`: `0xFFE7F` / `0x80FFE7F`) | MMIO writes | `writel(0xFFE7F, dma_base+off)` etc. | **direct** |
| Interrupts (`handleInterrupt`, Bitfile status/ack) | `IOInterruptEventSource` | `request_irq` (INTx) or `pci_enable_msix`; same read-status→ack-`(val&7)`→branch logic | rewrite plumbing, reuse protocol |
| SPI flash program (`spiprogram`/`spierase`, opcodes 0xEEEE / (len<<16)\|0xAAAA / 0xBBBB) | MMIO command regs on Flash region | same `writel` sequence — **not a real SPI bus, it's MMIO** | **direct** |
| Userspace API (`AppleAfterburnerUserClient`: submitFrame/queryCompletion/bindFrame/checkForSpace/cardStatus/getPercent) | IOKit UserClient + externalMethod | chardev (`miscdevice`) with `ioctl`s, or UIO if DMA stays in userspace | rewrite interface, reuse semantics |
| Registry properties (board rev, S/N, image rev, thermal) | IOKit registry | sysfs attributes under `/sys/class/afterburner/` | direct |

**Bottom line:** the register-level code (FINDINGS.md §7–9) is essentially copy-paste into C with
`readl/writel`. The genuinely new engineering is (a) BAR→region mapping, (b) Linux DMA API for the
ring + frames, (c) IRQ plumbing.

---

## 3. Architecture of the Linux driver

```
┌─────────────────────────── kernel module: afterburner.ko ───────────────────────────┐
│                                                                                      │
│  pci_driver { id_table=[106b:0205], probe, remove }                                  │
│        │                                                                             │
│        ▼  probe()                                                                    │
│  struct ab_device {                                                                  │
│     struct pci_dev *pdev;                                                            │
│     void __iomem *bar0;   /* ioremap'd, 128K — control/status regs */                │
│     void __iomem *bar1;   /* ioremap'd, 64K  — DMA staging / 2nd bank (TBD) */       │
│     u32 irq;                                                      │
│     struct ab_dma { dma_addr_t ring_pa; void *ring_va; int nslots; ... };           │
│     spinlock_t lock;   /* mirrors the kext's "Gated" submit path */                  │
│  };                                                                                  │
│                                                                                      │
│  Region accessors (port of FINDINGS.md §5–9):                                        │
│     ab_read_bar0(off) / ab_write_bar0(off,val)                                       │
│     ab_read_dma(off)   / ab_write_dma(off,val)      /* doorbells here */             │
│     ab_read_flash(off) / ab_write_flash(off,val)    /* SPI cmd block */              │
│     ab_read_bitfile/thermal/version/decode(...)                                        │
│                                                                                      │
│  IRQ handler: read Bitfile status → ack (val&7) → dispatch (bitfile/thermal/reconfig)│
│                                                                                      │
│  chardev /dev/afterburner0 + ioctls:                                                 │
│     AB_IOC_SUBMIT_FRAME, AB_IOC_QUERY_COMPLETION, AB_IOC_BIND_FRAME,                 │
│     AB_IOC_UNBIND_ALL, AB_IOC_CHECK_SPACE, AB_IOC_CARD_STATUS, AB_IOC_GET_PERCENT    │
└──────────────────────────────────────────────────────────────────────────────────────┘
        ▲ ioctl / mmap (ring) 
        │
   userspace app (frame producer/consumer)
```

Design choice — **where DMA lives**:
- **Option A (recommended first):** all DMA in kernel. Userspace passes frame buffers via
  `mmap`/`ioctl`; the module does `dma_map_single`, fills descriptors, rings the doorbell, and
  reports completion through a wait queue / poll. Mirrors the kext most faithfully.
- **Option B (later):** UIO — expose BARs + ring to userspace, do descriptor fill in user code.
  Faster iteration on the DMA path but you reimplement more of what IOKit gave for free.

Start with A; keep the register accessors identical so a later switch to B is mechanical.

---

## 4. Phased build plan

### Phase 0 — Bring-up & discovery (read-only, safe)
Deliverable: `afterburner_probe.c` that only *reads* and logs. No writes yet.
1. Build against running kernel: `make -C /lib/modules/$(uname -r)/build M=$PWD modules`.
2. In `probe()`: `pci_enable_device`, `pci_request_regions`, read **all** BAR sizes, ioremap both.
3. Read the **Version region** first (FINDINGS.md §5) — it's the safe "is the card alive" probe:
   - board revision word, S/N string, image revision (`%u.%u`), image type (live/golden/debug/HWTE).
4. Read Thermal region ADCs (0V85 VRM, backcard/bottom/faceplate/FPGA die) — confirms MMIO works.
5. **Do NOT** touch Flash or doorbells in this phase. Log everything to `dmesg`.
6. Success = we see plausible board rev / S/N / image revision matching what the macOS kext logged.

   *This also resolves the BAR→region map: whichever ioremap'd base returns sane version/thermal
   values is the one our FINDINGS.md offsets belong to.*

### Phase 1 — Register accessor layer (port, no behavior change)
Deliverable: `afterburner_regs.h/.c` — thin `readl/writel` wrappers for every region in
FINDINGS.md §5–9. Pure port; unit-test by reading back known status registers.

### Phase 2 — IRQ plumbing
1. Determine INTx vs MSI once device is enabled (`pci_msi_vecs_count`).
2. `request_irq` with the Bitfile-region handler: read status, ack `(val & 7)`, branch on which
   source (bitfile / thermal / reconfig / pointer-restore) per FINDINGS.md §8.
3. Validate by triggering a benign event and confirming the handler fires + acks cleanly.

### Phase 3 — DMA ring + frame submit (the hard part)
1. `dma_alloc_coherent` for the descriptor ring (slot count from the kext's ring size; head ptr at
   region offset `+0xc8`, 32 slots per FINDINGS.md §7).
2. Port `getDescriptorList`: build 32-byte descriptors, tag `0xAD4B`, with DMA addresses of frame
   buffers (`dma_map_single`).
3. Port `submitDataGated` under a spinlock: write descriptor words to the DMA region, then ring
   doorbell `writel(0xFFE7F, dma_base+off)` (single-shot) or `0x80FFE7F` (batch).
4. Completion: poll/IRQ on completion status; expose via wait queue.
5. **This phase needs the real physical addresses right** — the one place IOKit's address
   translation doesn't transfer. Iterate with a single small frame first.

### Phase 4 — Userspace interface
1. `miscdevice` `/dev/afterburner0`, ioctls mirroring the UserClient API (submitFrame,
   queryCompletion, bindFrame/unbindAll, checkForSpace, cardStatus, getPercent).
2. sysfs: board revision, S/N, image revision/type, thermal sensors.
3. Small C test client that submits a frame and waits for completion.

### Phase 5 — Flash / bitfile management (optional, last)
**Kext-side RE is COMPLETE — see FINDINGS.md §20.** Key facts for the port:
- SPI cmd block = BAR0+0x3000 (FLASH slot of the descriptor chain): status@+0x10, addr@+0x40, cmd@+0x48, data@+0x4C, done@+0x54.
- Update is two-phase across interrupts: live→golden (`CONTROL` BAR0+0x2000, +4 = `0x19000`), then on the golden-boot interrupt: erase + program both bitfiles at flash addr **`0x1400000`**, then live switch (+4 = `0x1A000`).
- Bitfile selection from fuse user word (STATUS+0xC): low2==3 → 2.62 EC family (`ec%02X` = bits 23:16), else plain 2.63; primary/secondary pair, max 2 attempts.
- `change_pointer()` writes the kext's embedded live/golden pointer tables (magic 0xA9560000) to flash — needed before reprogramming so the FPGA can still boot golden.

Only if we need to update the on-card FPGA image from Linux:
1. Port `spierase`/`spiprogram` (opcodes 0xEEEE erase, `(len<<16)|0xAAAA` program, busy-poll on
   flash[+0x10]).
2. Port the live/golden reconfig + pointer-restore logic from FINDINGS.md §9 + §20.4.
3. **High risk** — a bad flash write bricks the card's FPGA image. Gate behind an explicit
   userspace flag and only after Phases 0–4 are solid.

---

## 5. Build / packaging (Arch)

```
afterburner/
├── Makefile            # Kbuild: obj-m += afterburner.o ; afterburner-y := *.o
├── afterburner_pci.c   # probe/remove, BAR map, IRQ
├── afterburner_regs.c  # region accessors (port of FINDINGS.md)
├── afterburner_dma.c   # ring + submit + completion
├── afterburner_chardev.c # ioctls + sysfs
├── include/afterburner.h
└── userspace/abctl.c   # test client
```

- Build: `make` (Kbuild against `/lib/modules/$(uname -r)/build`).
- Load: `sudo make install && sudo modprobe afterburner`.
- Match is by PCI ID, so it binds automatically to `0f:00.0` on boot — **no EFI involvement**
  (UEFI only affects how the kernel itself boots; PCI enumeration + driver binding are identical).
- For reproducibility across Arch kernel updates: ship as an AUR package or DKMS module.

---

## 6. Risks & mitigations

| Risk | Mitigation |
|---|---|
| BAR→region map wrong (offsets land in the wrong BAR) | Phase 0 reads version/thermal on *both* ioremap'd bases; only proceed once one returns sane values. |
| DMA physical addresses wrong → card hangs / no completion | Start with one tiny frame, coherent ring, verify descriptor tag + doorbell via status readback before scaling up. |
| IRQ is INTx-only and shared/misrouted | Confirm vector after `pci_enable_device`; fall back to polling the Bitfile status register (the kext itself polls in several paths). |
| Flash write bricks FPGA image | Phase 5 last, explicit opt-in flag, read-back verify before/after, keep a known-good bitfile. |
| Card expects a specific init order we haven't fully captured | Follow the kext `start()` sequence exactly (map BARs → zero region ptrs → dispatch table → version read → configure interrupts); log each step. |
| "Capabilities: <access denied>" hides MSI/PM/AER | Re-run `lspci -vvv` *after* our module owns the device to see full capability list; adjust IRQ strategy accordingly. |

---

## 7. Handoff — where things stand (updated Oct 30)

**Live driver state:** `afterburner_submit2.ko` is the active test module (`linux-driver/`).
⚠️ **Card at `0f:00.0` is DEAD AGAIN (2026-11-03, run 14b)** — AER fatal MalfTLP during mode=14b's OWN
bring-up phase (~9 s after insmod, mid-decode), NOT post-completion idle as first assumed: the trigger is
the **full-size 33 MB receive-chain configuration itself** (both large-chain runs crashed regardless of the
disable write). MalfTLP = 64-byte MEM WR to 0x3CCCFFF0 = our desc-buffer base + 0x7FF0 (tail of the 32 KB
chain alloc) — see **FINDINGS.md §27.4**. Same all-F-BARs death signature; needs a full power cut / reboot.
Liveness checks MUST include dmesg/AER, not just register polling (registers read "alive" even post-fatal-AER).
Root-cause hypothesis + the kext's actual stop command: **FINDINGS.md §26**; kext DMA staging internals
(IODMACommand, maxSegmentSize ≈ 16 MB → ~3 descriptors for a 33 MB frame): **§27**. Key finding: kext
`IOService::stop()` override (fn@0xc0a4, vtable slot 191) writes exactly ONE register on teardown —
**BAR0[hdr+4] = 0 (decoder channel disable)**; BAR1+0x2004 engine-enable is never cleared anywhere in the
kext. Our driver leaves decoder enabled + idle after completion — a state that never occurs on macOS (VT
feeds frames continuously or disables via stop()).
Working submit path = **batch/receive channel**: stage descriptor (or chain head) PA into
BAR1+0x5080..C, ring BAR1+0x1004 with 0x80FFE7F; completion at +0x2044 (bit0 valid, [31:1] count),
data words +0x44/+0x48 (count==0) or +0x1044/+0x1048 (count≥1, batch index in BAR0[hdr+0x10]),
ack via write-back to +0x2018 (+0x2014). Send channel (+0x4080 / doorbell +4 = 0xFFE7F) does NOT
complete for chain submits — unresolved.

**Next actions (in order):**
0. **PHASE 3U — driver rewrite from decoded protocol (§8, plan of record).** Static RE is complete;
   one production module replaces the test-module pile, built byte-exact from FINDINGS §21–§30 +
   UC_METHODS §1–§21. The pending mode-14/15 live tests move into the rewrite's validation matrix
   (§8.5 V3/V6) and run via ioctl outside `.probe` — do NOT re-run them from submit2.ko's probe path
   (§28: long sequences in probe delayed AER recovery ~80 s and killed the card).
1. ~~Phase 3q (static): disassemble the vtable[+0x150] method...~~ → **RESOLVED (2026-11-02, FINDINGS.md §25):**
   premise was wrong — vtable[+0x150] is IOKit return-to-pool. Real commit chain: UC trampoline @0xd708
   → submitFrame @0xa9ac → submitData @0xa67c; doorbell fn@0x6800 byte-verified (channel cmd[+0x1c] from
   userland scalar; send = clear +4/stage +0x4080..C/ring 0xFFE7F, batch = clear +0x1004/stage
   +0x5080..C/ring 0x80FFE7F — byte-exact match with our driver). BAR1 base = ctx->+0x130 (correction:
   +0x148 is BAR0+0x2000, not BAR1). Completion handler fn@0x985c writes BAR0[hdr+0xC] on send-channel
   completion — a firmware "frame done" signal we don't currently write (check live for multi-frame).
2. **Find the decoded-output path:** where does ProRes YUV land after a successful chain submit?
   Look at the receive-side DMA descriptors / BAR0 channel blocks (+0x8000..+0xf000) and any
   user-buffer mapping in fn@0x9c5c's caller (the frame wrapper ~0xa698).
   → **RESOLVED statically (2026-11-01, Phase 3r):** provider `AppleAfterburner::submitFrame`
   takes an *output* IOMemoryDescriptor; the UC wires input+output descriptors per slot and the
   engine c2h-DMA's decoded YUV directly into the user buffer bound via `bindFrame` (per-slot DMA
   qword pair at UC+0xf8+i·0x18). See docs/videotoolbox-afterburner-decode-path.md §8. Live
   confirmation still pending (item 3 below).
3. **Live:** re-run mode=9 R9-1 with a longer poll + BAR0 channel-block snapshot before/after to
   catch decode-side activity; try count≥1 batch completions deliberately (multiple submits in one
   rung) to exercise the +0x1044 path and BAR0[hdr+0x10] index. → **folded into §8.5 V4** (runs via
   ioctl after the rewrite, not from submit2.ko's probe).
4. **Static open items** (FINDINGS.md §22 "UNRESOLVED"): u32 array @0x1806c, MetaClass vtable
   reconciliation, ~~dispatch table @0x15680 named list~~ — **RESOLVED 2026-11-01**: it is the UC's
   `sMethods` external-method dispatch table (7×24B records: submitFrame/queryCompletion/bindFrame/
   cardStatus/checkForSpace/unbindAll/getPercent; fields = trampoline fn, scalar-in count, input
   struct size, output qwords). Full ABI in docs/videotoolbox-afterburner-decode-path.md §3.
   x86_64 NULLs / −2 slot shift remain open.
5. **Phase 4** (userspace interface) and **Phase 5** (flash/bitfile port — RE done in §20) remain.

> Safety: Phase 0 is read-only (no doorbells, no flash). It cannot alter card state beyond
> enabling the device + mapping its BARs, which any driver must do anyway. Flash writes stay
> last and opt-in.

**Phase 3r (2026-11-01) — VideoToolbox ↔ kext decode path consolidated:**
docs/videotoolbox-afterburner-decode-path.md ties WWDC20-10090 + third-party VT references
(FFmpeg, sxplayer, dotnet/macios, docs.rs, Apple docs) to the fully decoded UserClient ABI:
externalMethod override @0xcfc8 → sMethods @0x15680; trampoline mechanism (IOLock@UC+0x1960,
PAC vptr verify tag 0xcda1, arg block scalar@+0x20/struct@+0x30/out@+0x48); live dispatch = raw
vtable slots 308–314 (Itanium +2 offset — earlier "slot 306" was an indexing error); submitFrame
full control flow incl. per-slot model (N=UC->+0x18f0, slotObj@UC+0xf0+i·0x18, DMA pair@+0xf8),
error codes (base 0xE000_02C2; no-space 0xE000_02D8); provider engine symbol map + log-string
behavior (hang watchdog, thermal frame drops, h2c/c2h DMA health). All 43 PLT stubs resolved.

**Phase 3t (2026-11-08) — Vtable architecture + static-init table closed out (batch 3):**
FINDINGS.md §30 + UC_METHODS.md §21. ADRP encoding settled via Keystone ground truth
(332/332 cross-check; immhi is 19 bits, mask 0x7FFFF — the "20-bit" alarm was false). All four
vtables bounded with live-vptr bases (AB inst 0x141F8 / AB MC 0x14A90 / UC inst **0x14BA8** /
UC MC 0x15588; two zeroed pre-virtual qwords each; virtual #v = raw slot v+2). Corrections:
fn@0xcfc8 IS in the UC instance table (raw slot 274 @0x15428 = virtual #272); BSS[0x10198]/
BSS[0x101C0] are in-place metaclass objects built by fn@0xcacc/fn@0xe770 via the static-init
table @0x141B8–0x141DF (loader-invoked, no BL callers); UC instance table has NO sentinel;
destructors are [D1,D0] order; AB instance = 7 genuine overrides + 6 PAC forwarders. Table `mid`
field verified as the dispatch PAC key. Remaining static open items: u32 array @0x1806c, x86_64
NULLs/−2 slot shift, class-descriptor integers (0x22/0x12). No driver changes — pure RE record.

---

## 8. Phase 3u — Driver rewrite from decoded protocol (plan of record, 2026-11-08)

Static RE is complete (batches 1–3; FINDINGS §21–§30 + UC_METHODS §1–§21). Every method the driver
needs is byte-verified. The incremental test-module approach (`afterburner_*.ko`, modes 0–15) has
served its purpose and is now a liability: `submit2.c` is ~1600 lines of mode switches, and §28
proved that running long sequences inside `.probe` can kill the card. This phase replaces the pile
with one production module built directly from the decoded protocol.

**Status (2026-11-09):** **3u.1 CLOSED — acceptance PASSED.** Card revived by full cold boot
(FINDINGS §31.1); 6 probes / 5 rmmod cycles all clean: fw gate `0x0002003e`, decoder header at
BAR0+0x8000, ready bit `0x3e0fd001` every time, stop verified on each remove, zero AER events.
Next: **3u.2 — submit via ioctl** (`ab_dma.c`: chain builder + doorbell + completion poll;
`ab_chardev.c`: /dev/afterburnerN mirroring the UC sMethods ABI).

### 8.1 Scope — v1 in / out

**In:** full bring-up (fw gate → dir walk → decoder enable → init writer), single-shot + chain
submit on the batch/receive channel, completion poll/ack, c2h output into user buffers, per-frame
reset, stop command on teardown, `.err_handler`, chardev + ioctls mirroring the UC ABI, sysfs,
userspace client.

**Out (deferred):** send-channel semantics (live-dead for chains — modes 10/11; implement the
kext-exact path but default to batch/receive), `pointer_restore_handler` port (§29.1 — link-event
recovery), flash/SPI update (Phase 5, opt-in, last), MSI completion IRQ (poll first — the kext
itself polls in several paths).

### 8.2 Architecture

Single module `afterburner.ko` under `linux-driver/afterburner/` (plan §5 layout):

| file | responsibility | decoded source |
|---|---|---|
| `ab_pci.c` | probe/remove/err_handler, BAR0+BAR1 map, MSI alloc. **Probe stays short** — no test sequences (§28) | start() @0x4254 |
| `ab_regs.h/.c` | named constants + readl/writel accessors for every decoded offset | FINDINGS §7–§9, §21; UC_METHODS §13 |
| `ab_engine.c` | bring-up: fw gate (BAR0+0x8 ∈ {0x0002003E, 0x0002003F}), dir walk from BAR0+0x0 (type=W&0xFF, next=O+((W>>14)&0x3FFFC), stride 0x2C), decoder enable/disable (BAR0[hdr+4]), ready poll (+0x8008 bit0), init writer fn@0x5A3C byte-identical, stop command (fn@0xc0a4: BAR0[hdr+4]=0), re-bringup recovery | §21.6, §29.1–§29.2, §26 |
| `ab_dma.c` | chain builder (byte-exact @0xa0f0–0xa164), staging banks +0x5080..C / +0x4080..C, doorbells 0x80FFE7F@+0x1004 / 0xFFE7F@+4 (fn@0x6800), completion poll/ack (+0x2044 → write-back +0x2018/+0x2014), pre-submit write BAR0[hdr+0x18]=0x1F (@0xa714), per-frame reset (UC_METHODS §10.3) | §25, UC_METHODS §15, FINDINGS §21.10 |
| `ab_chardev.c` | /dev/afterburner0 ioctls (UC ABI mirror) + sysfs attrs | UC_METHODS §3–§6, §8 |
| `userspace/abctl.c` | test client: bind buffer → submit fixture frame → poll completion → dump output | — |

**ioctl set** (mirrors sMethods @0x15680, UC_METHODS §3): SUBMIT_FRAME, QUERY_COMPLETION,
BIND_FRAME, UNBIND_ALL, CHECK_SPACE, CARD_STATUS, GET_PERCENT + driver-specific BRINGUP / STOP /
RECOVER. Per-slot model: N slots (kext count from UC->+0x18f0), slot i owns input PA/len + output
buffer pair — same shape as the kext's per-slot DMA qword pairs (@UC+0xf8+i·0x18). Error codes map
to the kext table (base 0xE000_02C2, no-space 0xE000_02D8; UC_METHODS §8) so userspace behavior is
identical.

**sysfs:** board rev, S/N, image rev/type, thermal ADCs (4 sensors), decoder ready bit, engine
state words (+0x2010/+0x2044 read-only views).

### 8.3 Hard constraints (live-verified — non-negotiable)

1. **≤126 output descriptors per chain** (fw cap; 257 → deterministic MalfTLP, §28.1). Chain
   builder hard-errors above the cap; default `out_seg=4MB` keeps a 33 MB frame at 8 descs.
2. **Never run long sequences inside `.probe`** (§28.2 — one card kill). All test/decode work is
   ioctl-triggered in process context, interruptible, with bounded poll windows.
3. **Engine-owned registers:** +0x2018/+0x2014 writes are ignored (readback ≠ write) — never try to
   clear them; recovery = decoder re-bringup cycle or FLR (§26/§3n).
4. **Decoder enable persists across rmmod/insmod** → bring-up must be idempotent and handle the
   already-enabled state (ready bit may already be set on a warm card).
5. **BAR0+0x202C is not writable** — kext writes it, readback stays 7; write-and-ignore with a
   comment (§3k live run).
6. **Liveness checks include dmesg/AER**, not just register polling (registers read "alive" even
   post-fatal-AER, §28).
7. PAs fit in 32 bits on this box (observed 0x3a400000–0x46xxxxxx; kext numAddressBits=0→32) —
   keep ring + buffers below the 4 GiB PA boundary or use a 32-bit DMA mask.

### 8.4 Phases and acceptance criteria

**3u.1 — Core skeleton.** ab_pci (short probe, err_handler that logs), ab_regs, ab_engine
(bring-up + stop command on remove). No submit path yet.
*Accept:* insmod/rmmod ×5 clean; dmesg silent (no AER); decoder ready readback correct after
bring-up; BAR0[hdr+4]=0 verified after rmmod (stop command ran).
**✅ PASSED 2026-11-09** — FINDINGS §31.1 (6 probes / 5 cycles, zero AER, warm=0 every probe).

**3u.2 — Submit path via ioctl.** ab_dma: single-shot len=64 → real-frame chain on batch/receive;
completion poll + ack; per-frame reset; RECOVER ioctl (re-bringup cycle first, FLR fallback).
*Accept:* len=64 consumed + status VALID (regression vs §3i); 320×180 fixture chain completes with
sane data words (§3o R9-1 parity); **H1/H2 test** — full frame `out_seg=4MB` (8 descs) + stop
command + ≥75 s idle, AER-checked, run outside probe (the pending mode-14/15 question, now safe).
**🟡 IN PROGRESS 2026-11-09:** fixture chain via batch/receive WORKS — 4× identical frames, all
complete with status=0x1/data=0x6, output verified against FFmpeg reference decode (luma 100%
within ±1; chroma = half-vertical even rows only, deterministic — FINDINGS §32.5). Multi-frame
garbage root-caused + fixed: per-frame `AB::reset()` parity + cmd_word delta semantics (§32.3/§32.4).
Remaining: len=64 regression re-run on the production module (V1), H1/H2 4K test (V3), RECOVER ioctl.

**3u.3 — Userspace.** chardev + sysfs + abctl client.
*Accept:* userspace round-trip of the fixture frame; decoded output buffer matches an FFmpeg decode
of `testdata/prores_320x180_frame0.bin` (first real end-to-end decode verification).
**🟡 IN PROGRESS:** chardev + mmap (UC, No-Snoop-safe) live and exercised by `tests/abtest`; the
FFmpeg-reference match above was done against abtest's per-frame dumps — first real end-to-end
decode verification achieved. Remaining: sysfs surface, abctl client proper.

**3u.4 — Hardening.** Hang watchdog per sleep_timer decode (UC_METHODS §17: deadline poll + reset
on hang + decoder disable on exit); multi-frame batching across the 8 channel blocks; count≥1 batch
completions (+0x1044 path, BAR0[hdr+0x10] index — plan §7 item 3); long idle soak.
*Accept:* no wedge after N consecutive frames; watchdog fires + recovers on an induced stall; the
destructive control run (no stop command) goes LAST in the suite per mode-15 design.

**Phase 5** (flash/bitfile port, RE done §20/§29.3) stays last and opt-in.

### 8.5 Validation matrix (live runs after rewrite; destructive LAST)

| # | test | purpose | status (2026-11-09) |
|---|---|---|---|
| V1 | len=64 single-shot + VALID ack | regression baseline (§3i) | ⬜ pending on production module |
| V2 | fixture frame chain, batch/receive | §3o R9-1 parity via the new code path | ✅ **DONE** — 4× identical frames complete (status=0x1/data=0x6), output verified vs FFmpeg reference (FINDINGS §32.5) |
| V3 | full 4K ProRes frame (§24), `out_seg=4MB` (8 descs), stop cmd, ≥75 s idle + AER check | **H1 vs H2** (§27.5) — mode-14 equivalent | ⬜ pending |
| V4 | multi-frame ×N across channels; count≥1 batch completion (+0x1044 path) | plan §7 item 3 | 🟡 partial — sequential multi-frame ×4 verified (FINDINGS §32); cross-channel + count≥1 paths remain |
| V5 | induced stall → watchdog reset/re-bringup | UC_METHODS §17 port check | ⬜ pending (3u.4) |
| V6 (destructive, last) | full frame, NO stop command, ≥75 s idle + AER watch | mode-15 control — expected to die; confirms stop-command necessity | ⬜ pending (LAST) |

### 8.6 Risks & mitigations (updated)

| risk | mitigation |
|---|---|
| AER during a long ioctl test | no dev->mutex held across the work (process context); err_handler implemented; bounded poll windows; V3/V6 run with a parallel dmesg watcher |
| fw 126-desc cap hit by user input | chain builder hard-errors above cap; out_seg clamped so descs ≤ 126 for any frame size |
| wedged engine mid-test | RECOVER ioctl: re-bringup cycle first (rung A, §3n), FLR second; power-cycle documented as last resort |
| decoder already enabled from a prior session | idempotent bring-up (ready-bit check before the enable cycle) |
| output layout mismatch | card outputs interleaved YUV422 **8-bit** (`yuv422p` packed), NOT planar p10le (§26.2); abctl verifies against an FFmpeg reference decode in V2/V3 |

### 8.7 Housekeeping
- Commit the uncommitted modes-14/15 diff in `afterburner_submit2.c` (test-design record; superseded by V3/V6).
- Old test modules stay in-tree as history but get a deprecated header note once 3u.1 lands.
