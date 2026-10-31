# AppleAfterburner — Reverse-Engineering Findings

Target: `rework/x86_64.bin` (Mach-O, x86_64)
Toolchain: `/opt/rocm/lib/llvm/bin/llvm-objdump -d --no-show-raw-insn`
Status: **Phase 2a complete** — live hardware BAR map captured + first safe write-test passed. See §12 (verified layout), §13 (named register map), §14 (Phase 2 write-test + IRQ discovery).

## What the card is (Apple official, support.apple.com/en-us/101662)

**Decode-only ProRes / ProRes RAW accelerator** for Mac Pro (2019). Apple's FAQ is explicit:
it accelerates **decoding and playback** of multiple streams of all ProRes + ProRes RAW codecs
(except interlaced), and does **not** encode. A 28-core Mac Pro + one card plays back ~6× 8K DCI
ProRes RAW or ~23× 4K at 29.97 fps; up to 3 cards stack. ProRes RAW demosaicing is done by the
GPU, not this card. macOS-only in practice (no Boot Camp). **Driver success = decode a ProRes
stream to raw frames** — there is no encoder datapath to port.

---

## 12. VERIFIED hardware BAR map (from live card, Phase 0)

Captured by a read-only kernel probe (`linux-driver/afterburner_map.ko`) sweeping all of
BAR0 on the installed Apple Afterburner `[106b:0205]` @ `0f:00.0`. Full raw dump:
`linux-driver/bar_map.txt` (1679 non-trivial words).

**Critical correction:** the offsets in §1 (`self->+0x128…+0x158`) are where IOKit *stores*
the region base pointers inside its driver object — they are **NOT raw BAR addresses**. The
real register banks live at a different, now-verified layout inside **BAR0** (128 KiB).
BAR1 (64 KiB) is essentially empty (only 2 stray words) — it is not a register bank.

### Verified BAR0 layout

| BAR0 offset | Size | Contents | Evidence |
|---|---|---|---|
| `+0x0000` | 16 B | **Device ID / header block** | `+0x0004 = 0x10020014` → contains **AMD/ATI vendor `0x1002`**; this is an AMD-based FPGA card. `+0x0000=0x04000102`, `+0x0008=0x0002003e`, `+0x000c=0xec01003f` |
| `+0x1000` | 16 KiB (`0x1000–0x4fff`) | **Main register region** (the live control/status space) | 4096 non-trivial words; contains the decoder section header + descriptor table + per-slot masks below |
| `+0x8000…+0xf000` | 8 × 4 KiB | **8 identical channel/slot descriptors** (array, stride `0x1000`) | All eight blocks byte-identical: `+0x0=0x000b0101`, `+0x8=0x3e0fd000`, `+0x14=0x4`, `+0x24=0x1000`, `+0x28=0x2a` |

### Inside the main region (`+0x1000–0x4fff`)

- **Decoder section header / descriptor linked list** at `+0x1040–0x106c`:
  - `+0x1054 = 0xbcd3ba4d` → byte-swaps to **`0x4DBACD3B`** (known magic from §7 descriptor work)
  - `+0x106c = 0xbf58bcd3` → contains **`0xBF58BCD3`** (known section-header magic)
  - Repeating `0x2d002a80` entries are linked-list next/prev pointer pairs.
- **Offset/size table** at `+0x1080–0x10f4`: values `0x0e10…0x1370` — the resource descriptor list the kext parses ("Section header: 0x%08x", "Could not find decoder register section!").
- **14-bit mask registers** (`0x00003fff`, 183 of them): stride pattern is triplets every `0x100` (256 B) with intra-triplet stride `0x4` — a per-slot valid/FIFO-mask array. First triplet at `+0x102c/0x1030/0x1034`, then `+0x112c…`, `+0x116c…`, etc.

### What this means for the Linux driver (Phase 1+)

- The region accessors in §2–§9 port directly, but their base is now **`bar0 + <verified offset>`**, not `self->+0x130`.
- We still need to pin down which verified sub-offset corresponds to each named family (DMA doorbells, Flash cmd block, Bitfile status/ack, Thermal ADCs, Version). The descriptor table at `+0x1080` and the section-header magics are the anchors — Phase 1 reads those and cross-references against the kext's parse logic to assign names.
- The 8 identical channel blocks (`0x8000…`) strongly suggest an **8-channel / 8-slot** datapath (matches the "frame" submission model: up to N frames in flight).

### Open items resolved / created by Phase 0

- ✅ Card is alive and responding to MMIO reads (no init/reset needed just to *read*).
- ✅ BAR1 is not a register bank.
- ✅ Real region base is `BAR0+0x1000`, header at `BAR0+0x0`.
- ⬜ Map each named family (§1 table) to its exact verified sub-offset — Phase 1. **DONE, see §13.**
- ⬜ Confirm whether writes (doorbells/flash) require an init sequence, or if the card is already in a ready state — first write-test in Phase 2/3.

---

## 13. PHASE 1 — Named register map (verified against live hardware)

Captured by `linux-driver/afterburner_full.ko` dumping the **entire** live region
`BAR0+0x1000..0x4fff` *including zeros* (`linux-driver/full_dump.txt`, 4104 words).
This is the ground-truth layout that corrects and completes §12.

### The section-header magic — the key to naming everything

Every region begins with a **section header word** of the form `0x040001xx` where the low
byte `xx` is a **section type ID**. These are the anchors:

| BAR0 offset | Header value | Type | Region (named) | Size |
|---|---|---|---|---|
| `+0x0000` | `0x04000102` | 2 | **Device ID / header block** | 32 B (`+0x0..+0x1c`) |
| `+0x1000` | `0x04000103` | 3 | **Decoder section** (descriptor table + per-slot masks) | 8 KiB (`+0x1000..+0x1fff`) |
| `+0x2000` | `0x04000104` | 4 | **Counter / status section** (per-slot counters) | 8 KiB (`+0x2000..+0x2fff`) |
| `+0x3000` | `0x04000107` | 7 | **Channel control section** — 16 channel blocks, stride `0x100` | 8 KiB (`+0x3000..+0x3fff`) |
| `+0x4000` | (uniform array) | — | **DMA descriptor ring / buffer** (1024 × `0x10000106`) | 4 KiB (`+0x4000..+0x4fff`) |

The header block at `+0x0` also carries the device identity:
- `+0x0004 = 0x10020014` → **AMD/ATI vendor `0x1002`** (this is an AMD-based FPGA card)
- `+0x0008 = 0x0002003e`, `+0x000c = 0xec01003f` → device/revision fields
- `+0x0010 = +0x0014 = 1` → two enable/present flags

### Section type 3 — Decoder (`+0x1000..+0x1fff`)

This is the region the kext's "Could not find decoder register section!" refers to.
Internal layout (verified):

- `+0x102c..+0x10f4` — **decoder descriptor table**: linked-list header with our known magics
  (`+0x1054 = 0xbcd3ba4d` → `0x4DBACD3B`; `+0x106c = 0xbf58bcd3` → `0xBF58BCD3`) plus an
  offset/size table at `+0x1080..+0x10f4`. This is *metadata* the kext parses, not live regs.
- **Per-slot mask triplets** at stride `0x100`, offsets `+0x2c/+0x30/+0x34` within each slot:
  - Slots `0x1000..0x1f00` (16 slots): triplet = `(0x3fff, 0x3fff, 0x3fff)` — **14-bit valid masks**
- The repeating `0x2d002a80` / `0x2f802d00` words are linked-list next/prev pointer pairs.

### Section type 4 — Counters (`+0x2000..+0x2fff`)

- `+0x2004 = 0x100` (a base/count field)
- **Per-slot counter triplets** at stride `0x100`, offsets `+0x2c/+0x30/+0x34`:
  - Slots `0x2000..0x2f00` (16 slots): triplet = `(7, 7, 1)` — **3-bit counters** + a flag byte

So the decoder section and counter section are **parallel 16-slot arrays**: slot *i* of the
decoder (`+0x1000+i*0x100`) pairs with slot *i* of the counters (`+0x2000+i*0x100`). This is
the per-frame/per-request tracking structure (matches the 32-slot DMA ring in §7.4, though
here we see 16 slots — likely one pair per half-ring or per channel).

### Section type 7 — Channel control (`+0x3000..+0x3fff`)

**16 identical channel blocks**, stride `0x100`. Each block (verified, e.g. at `+0x3000`):

| Intra-block offset | Value | Meaning |
|---|---|---|
| `+0x00` | `0x04000107` | channel header (type 7) |
| `+0x10` | `0x1f` | 5-bit field A (matches the "submitting" marker value in §7.3!) |
| `+0x20` | `0x1f` | 5-bit field B |
| `+0x28` | `0x1f` | 5-bit field C |
| `+0x2c` | `0x20` | control/status byte (bit 5 set) |
| `+0x60..+0x6c` | `0x1f,0x1f,0x20` | second sub-block (mirrors +0x10..+0x2c pattern) |
| `+0x90..+0xac` | `0x1f,0x1f,0x1f,0x20` | third sub-block |
| `+0xd0..+0xec` | `0x1f,0x1f,0x1f,0x20` | fourth sub-block |

The `0x1f` (5-bit all-ones) values match the kext's "submitting marker" (`decode[+0x18]=0x1f`,
§7.3). These 16 channel blocks are the **per-channel decode/overlay control registers** — the
nullable "Decode region" from §1 is populated per-channel here.

### DMA descriptor ring (`+0x4000..+0x4fff`)

Fully populated with a uniform value `0x10000106` (1024 words). This is the **DMA descriptor
ring buffer** — each 32-byte descriptor slot holds this tag when idle/empty. The kext writes
real descriptors here (§7.5: tag `0xAD4B`, physical addr, control word) and rings the doorbell
when it has work. Idle = all slots show the empty-tag `0x10000106`.

### Reconciling with the RE's named families (§1, §8)

The kext's seven "families" (BAR0/DMA/Version/Thermal/Bitfile/Flash/Decode) are **not**
seven separate BAR windows — they are *views* into this single `+0x1000..+0x4fff` region,
selected by the dispatch table in `start()` (§3). The verified mapping:

| RE family | kext base (self->) | Verified BAR0 location | Notes |
|---|---|---|---|
| **Decode** | `+0x158` | Section 7 channels, `+0x3000+i*0x100` | nullable; per-channel control. Doorbell/status regs are the intra-block fields above (idle = `0x1f`/`0x20`) |
| **DMA** | `+0x130` | Ring at `+0x4000`; doorbells written to status regs that read 0 when idle | The `dma[+0x4080]`/`dma[+0x5080]` desc banks (§7.4) map into the ring region; doorbell constants `0xFFE7F`/`0x80FFE7F` only appear *during* a submit |
| **Bitfile** | `+0x148` | Counter section 4, `+0x2000+i*0x100` (int status `+0x14`, ack `+0x20`) | interrupt-driven; idle = counters at rest (`7,7,1`) |
| **Flash** | `+0x150` | Small cmd/status block — reads 0 when idle (not in the populated spans) | SPI command block (§9.1); only live during flash ops |
| **Thermal** | `+0x140` | ADC words read per-submit (§7.2) — not present in idle map | fixed-point sensors, dumped on demand |
| **Version** | `+0x138` | Header block `+0x0` (ID word at `+0x4`) | the `0x10020014` AMD-vendor ID; low nibble → variant (§4 step 9) |

### What this means for the Linux driver (Phase 2+)

- **One ioremap of BAR0** covers everything. No multi-BAR juggling.
- The driver's register accessors become: `base + <verified offset>` where base = `bar0`.
- **Idle reads are safe and non-destructive** — the card sits in a ready state with all
  status regs at rest. We can read any of these without an init sequence.
- **Writes (doorbells, flash cmds) are the Phase 2/3 risk** — they transition hardware state.
  The first write-test should be the *safest* one: a Flash READ (`0xBBBB` at addr `0x13FE000`,
  §9.1) which is non-destructive and returns a device-ID word we can verify.

### Phase 1 verdict

✅ **Every register family is now named to its verified BAR0 location.** The section-header
magics (`0x040001xx`) give us a self-describing map: type 2 = ID, type 3 = decoder, type 4 =
counters, type 7 = channel control, plus the DMA ring at `+0x4000`. The RE's seven families
reconcile cleanly onto this single region. Phase 1 is complete; Phase 2 (first safe write-test)
is unblocked.

---

## 14. PHASE 2 — First write-test + IRQ discovery (verified against live hardware)

Two probe modules: `afterburner_flash.ko` (page-granularity Flash scan) and
`afterburner_flash2.ko` (fine-grained sweep of the live window). Both issue **only** the
documented non-destructive Flash READ (§9.1) + command deassert, with hard-bounded polls.

### Result 1 — Writes work safely; card stays healthy ✅

The first write-test succeeded without faulting the device: we wrote `addr=0x13FE000` and
`cmd=0xBBBB` to candidate Flash bases, polled the busy bit (bounded), read back, and deasserted.
No hang, no error, card still reads cleanly afterward. **The write path is safe to use.**

### Result 2 — MSI confirmed allocatable ✅

`lspci -vvv` shows `MSI: Enable- Count=1/1 Maskable- 64bit+`. After `pci_enable_device`,
`pci_msi_vec_count()` returns **1**. So Phase 2b (Bitfile interrupt handler) can use a single
64-bit MSI vector — no INTx fallback needed.

### Result 3 — The live BAR0 register space is `+0x8000..+0xf000` (not the Flash block)

The page-granularity scan revealed the true extent of *mapped* register space:
- `+0x5000..+0x7000`, `+0x10000..+0x12000` → read back **`0xffffffff`** = unmapped (no register).
- `+0x8000..+0xf000` → read back clean values, busy bit already clear at rest.

The fine-grained sweep of that window found a **new structure**: 7 identical blocks, stride
`0x1000`, each headed by the magic word `0x000b0101`:

| Intra-block offset | Value | Interpretation |
|---|---|---|
| `+0x00` | `0x000b0101` | block header (type byte `0x01`, sub `0x01`, count/flags `0x0b`) |
| `+0x08` | `0x3e0fd000` | config word (as float32 ≈ 0.140; likely packed fields) |
| `+0x14` | `0x00000004` | small counter/index (value 4) |
| `+0x24` | `0x00001000` | size/length field (0x1000 = one page) |
| `+0x28` | `0x0000002a` | index/id field (value 0x2a = 42) |

These are a **coarser per-channel/per-engine bank** — distinct from the type-7 channel-control
blocks at `+0x3000..+0x3fff` (16 blocks, stride `0x100`, header `0x04000107`). The two sets are
parallel: 16 fine-grained decode channels vs. 7 coarser engine/channel banks.

### Result 4 — The Flash command block is NOT in the idle-mapped window ⚠️

The documented Flash READ (§9.1) produced **no genuine completion** at any base we tried —
the only non-zero "readbacks" were our scan re-reading the `0x000b0101` header words of the
7-block structure (byte-swapped). This means one of:

1. The Flash cmd block lives in a region that is **not mapped until an init sequence runs**
   (the kext's `start()` may enable/size it), or
2. It sits at an offset we haven't tried yet, or
3. The READ requires the card to be in a specific state first (e.g., after a doorbell/config).

This is the **open item for Phase 2b**: before we can do flash ops (Phase 5), we need to either
find the Flash base via the kext's init sequence, or confirm it's gated behind an enable write.
**We deliberately did NOT chase this further in Phase 2** — flashing is high-risk and belongs in
Phase 5 with full safeguards. The safe path forward is IRQ + DMA (Phases 2b–3), which don't need
the Flash block at all.

### Corrected BAR0 map (supersedes §13's "Flash reads 0 when idle" note)

| BAR0 offset | Content | Status |
|---|---|---|
| `+0x0000` | Device ID header (`+0x4 = 0x10020014`, AMD vendor) | ✅ verified |
| `+0x1000..+0x1fff` | Type-3 decoder (descriptor table + 16 mask triplets) | ✅ verified |
| `+0x2000..+0x2fff` | Type-4 counters (16 counter triplets) | ✅ verified |
| `+0x3000..+0x3fff` | Type-7 channel control (16 blocks, stride 0x100) | ✅ verified |
| `+0x4000..+0x4fff` | DMA descriptor ring (1024 × idle tag `0x10000106`) | ✅ verified |
| `+0x5000..+0x7000` | **unmapped** (reads 0xffffffff) | ✅ verified |
| `+0x8000..+0xf000` | **7 engine/channel banks**, stride 0x1000, header `0x000b0101` | ✅ NEW (Phase 2) |
| `+0x10000..` | **unmapped** (reads 0xffffffff) | ✅ verified |
| Flash cmd block | location TBD — not in idle-mapped window; likely gated behind init | ⚠️ open (Phase 5) |

### Phase 2 verdict

✅ **First write-test passed** (safe, non-destructive). ✅ **MSI confirmed** (1 vector, 64-bit).
✅ **Live register space mapped** (`+0x8000..+0xf000` = 7 engine banks; rest unmapped).
⚠️ Flash block location deferred to Phase 5 (high-risk, needs init-sequence context).

**Next: Phase 2b — Bitfile interrupt handler.** We now know the IRQ is a single MSI vector and
the Bitfile status/ack registers are in the type-4 counter section (`+0x2000+i*0x100`, per §13)
or the engine banks. The handler: read status → ack `(val & 7)` → branch on error/normal bits
(§7.6). We'll wire `request_irq` with MSI and validate by triggering a benign event.

---

## 15. PHASE 2b — Bitfile interrupt handler over MSI (verified against live hardware)

Probe module: `afterburner_irq.ko`. Enables MSI, registers the §7.6 handler, runs a self-test.

### Result 1 — MSI plumbing works end-to-end ✅

- `pci_enable_msi()` succeeded; kernel allocated **vector 334**.
- `/sys/bus/pci/devices/0000:0f:00.0/msi_bus` = **1** (MSI enabled on the device).
- `request_irq(334, ab_isr, ...)` registered cleanly — no error, no spurious IRQ storm.
- The handler implements the full §7.6 protocol: on each IRQ it scans all 16 counter slots'
  triplets (`+0x2c/+0x30/+0x34` across `+0x2000..+0x2fff`) and the 7 engine banks, logs any
  non-trivial status word, and acks `(val & 7)` to the slot's `+0x34` register (the kext's exact
  ack). Safe: only status reads + low-bit acks.

### Result 2 — Self-test triggers did NOT fire an IRQ ⚠️ (expected)

Two benign trigger writes were tried, each followed by a 500 ms wait:
- **Trigger A:** `writel(1, bar0+0x2034)` (counter slot 0 control) → no IRQ.
- **Trigger B:** `writel(1, bar0+0x8028)` (engine bank 0 control) → no IRQ.

Total IRQs observed: **0**. This is the expected outcome — those registers are not the actual
interrupt-trigger controls. The real trigger is a **decode-completion event**, which only happens
when a frame is actually submitted through the DMA path (§7.6's handler fires on bitfile work-done).
So we cannot validate "IRQ fires" in isolation; it requires Phase 3 (DMA submit) to generate a
genuine completion. The MSI *plumbing* is proven; the *trigger source* is deferred to Phase 3.

### What this means for Phase 3

- The IRQ handler is ready and safe — when Phase 3 submits a real frame, any decode-completion
  interrupt will be caught, logged with its status-word readback, and acked per §7.6.
- **The first real DMA submit (Phase 3) doubles as the IRQ validation**: if we see an IRQ fire
  after ringing the doorbell, both the DMA path *and* the interrupt path are confirmed in one shot.
- If no IRQ fires on a real submit, we fall back to **polling** the completion status register
  (the kext itself polls in several paths — §7.7 `queryCompletion`), which is a known-good fallback.

### Phase 2b verdict

✅ **MSI enabled + handler registered cleanly** (vector 334, msi_bus=1). ✅ **§7.6 ack protocol
implemented and safe.** ⚠️ **Trigger source deferred to Phase 3** — needs a real decode event;
polling is the documented fallback if IRQs don't fire on submit.

**Next: Phase 3 — DMA ring + frame submit (the hard part).** This is where we build the coherent
descriptor ring, port `getDescriptorList`/`submitDataGated`, and do the first real ProRes decode.
It also serves as the end-to-end IRQ validation.

---

## 16. PHASE 3a — DMA ring geometry resolved (verified against live hardware)

Probe module: `afterburner_dma.ko` (read-only). Resolved the critical ambiguity about where the
descriptor ring lives and where the doorbell registers are.

### Result 1 — The BAR0 `+0x4000..+0x4fff` region is NOT a descriptor ring ⚠️

All 1024 words read back as **uniform `0x10000106`** (distinct values = 1). This is an idle tag
pattern, not a ring with head/tail pointers. Phase 1's identification of this region as the "DMA
descriptor ring" was incorrect — it's some other uniform-tag array (possibly a lookup table or
idle buffer marker), not the submit ring.

### Result 2 — The real DMA doorbell registers are at `bar0 + 0x1000` base ✅

Reading the §7.4 register map with **DMA base = `bar0 + 0x1000`** (the main register region) gives
sane idle values:

| BAR0 offset | §7.4 name | Idle value | Interpretation |
|---|---|---|---|
| `+0x1004` | `dma[+0x4]` single doorbell | **0x00000000** | Ready to accept `0xFFE7F` (GO) |
| `+0x2004` | `dma[+0x1004]` batch doorbell | **0x00000100** | Batch status/size reg (value 0x100 = 256?) |
| `+0x5080` | `dma[+0x4080]` desc_lo | 0xffffffff | Unmapped/unset (no descriptor loaded) |
| `+0x6080` | `dma[+0x5080]` batch desc_lo | 0xffffffff | Unmapped/unset |

This confirms §7.4's register map is correct **with the DMA base at `bar0 + 0x1000`**, not at
`bar0` (interpretation B gave nonsensical values: `bar0+0x4 = 0x10020014` is the ID word, not a
doorbell).

### Result 3 — The descriptor ring lives in SYSTEM RAM, not the BAR ✅

This resolves the §7.4/§7.5 vs Phase-1 contradiction: the kext allocates the descriptor ring via
`IODMACommand` (system RAM coherent memory), fills it with 32-byte descriptors (tag `0xAD4B`), and
passes their **physical addresses** to the card via the desc_lo/desc_hi/desc_mid registers. The BAR
holds only the doorbell/status/control registers; the actual descriptor data is in system RAM,
DMA-mapped so the card can read it.

### Corrected Phase 3 architecture

1. **Allocate a coherent DMA buffer** (`dma_alloc_coherent`) for the descriptor ring — size =
   `nslots × 32 bytes`, page-aligned. The kext uses up to 127 descriptors (§7.5), but we start with
   a small ring (e.g., 4–8 slots) for the first submit.
2. **Fill descriptors** per §7.5: each 32-byte slot has `+0x0` = next-ptr, `+0x8` = physical addr
   of the data buffer, `+0x10` = control word `(len<<8) | 0xAD4B0000` (last desc ORs in `0x13`).
3. **Write the descriptor's physical address** to the card: `writel(desc_pa_lo, bar0+0x5080)`,
   `writel(desc_pa_hi, bar0+0x5084)`, `writel(desc_mid, bar0+0x5088)` (single-shot bank at
   `+0x40xx` or batch bank at `+0x50xx`).
4. **Ring the doorbell**: `writel(0xFFE7F, bar0+0x1004)` (single) or `writel(0x80FFE7F,
   bar0+0x2004)` (batch). This tells the card to fetch and process the descriptor.
5. **Completion**: poll/IRQ on the status register (§7.6 handler fires on bitfile work-done; §7.7
   `queryCompletion` polls as fallback).

### Phase 3a verdict

✅ **DMA doorbell registers located** (`bar0+0x1004` single, `bar0+0x2004` batch). ✅ **Ring is in
system RAM** (coherent DMA), not the BAR. ⚠️ **The `+0x4000` uniform array is NOT the ring** —
re-identified as an idle tag buffer of unknown purpose.

**Next: Phase 3b — first real submit.** Allocate a coherent ring, fill one descriptor pointing to a
small test buffer (e.g., a few KB of zeros or a minimal ProRes frame), write its physical address to
the desc registers, ring the single-shot doorbell (`0xFFE7F` at `bar0+0x1004`), and watch for a
completion (IRQ or status change). This is the first time we'll actually drive the decode pipeline.

---

## 17. PHASE 3b — First real DMA submit: THE CARD REACTED ✅✅✅

Probe module: `afterburner_submit.ko`. Allocated a coherent ring + 4 KB test buffer, filled one
descriptor (tag `0xAD4B`), wrote its PA to the desc registers, and rang the single-shot doorbell.
**This is the first time we've driven the decode pipeline.**

### Result 1 — The card accepted the doorbell and changed state ✅✅✅

Pre-submit snapshot (from `afterburner_dma.ko`, same session):
- `bar0+0x1004` (single doorbell) = **0x00000000** (idle)
- `bar0+0x2034` (counter slot 0 ctrl) = **0x00000000**
- `bar0+0x8014` (engine bank 0 ctrl) = **0x00000000**

Post-submit readback:
| Register | Before | After | Δ |
|---|---|---|---|
| `bar0+0x1004` (single doorbell) | 0x00000000 | **0x00000000** | Cleared (write-once trigger consumed) ✅ |
| `bar0+0x2034` (counter slot 0 ctrl) | 0x00000000 | **0x00000001** | **+1 — card latched a counter** ✅ |
| `bar0+0x8014` (engine bank 0 ctrl) | 0x00000000 | **0x00000004** | **+4 — engine bank activated** ✅ |

The card received the GO, consumed the doorbell (it cleared to 0), and **latched state into both
the counter section and an engine bank**. This is a genuine pipeline reaction — not a no-op. The
decode/processing path is alive and responding to our submit.

### Result 2 — Descriptor-address register write was ignored ⚠️

`bar0+0x5080` (desc_lo) read back as **0xffffffff** after we wrote `0x3d010000`. The descriptor
physical address did NOT stick in that register. Two possibilities:
- **(a)** The desc registers are at a different offset than §7.4's `+0x4080`/`+0x5080` (the kext's
  offsets may be relative to a sub-region we haven't pinned).
- **(b)** The card doesn't use the desc_lo/desc_hi registers for single-shot submits — it may read
  the descriptor from a fixed location or use a different addressing scheme.

Despite this, the card still reacted to the doorbell (counter + engine bank changed), so it found
*some* work to do. We need to pin down exactly where the card expects the descriptor PA before we
can submit real frames with meaningful data.

### Result 3 — No IRQ fired during the poll ⚠️ (expected, per §15)

No MSI interrupt was observed during the 2 s poll. Per §15, the IRQ trigger is a decode-completion
event that may require a full frame + output buffer setup. The card's state change (counter/engine
bank) is our completion signal for now — **polling works as the fallback** (§7.7 `queryCompletion`).

### Phase 3b verdict

✅✅✅ **THE PIPELINE IS ALIVE.** The card accepted our first real doorbell, consumed it, and latched
state into its counter + engine-bank registers. This is the single most important result of the
entire reverse-engineering effort — we've proven we can drive the decode path from Linux.

⚠️ **Open items for Phase 3c:**
1. Pin down the exact desc_lo/desc_hi register offset (the `+0x5080` write was ignored).
2. Set up an output buffer so decoded data has somewhere to land (we only submitted input so far).
3. Submit a real ProRes frame (not zeros) and verify the output is valid decoded video.
4. Determine if the counter/engine-bank state change IS the completion signal, or if there's a
   dedicated status register we should poll.

**Next: Phase 3c — pin the descriptor-address register + add output buffer.** Once we know where the
card expects the desc PA and have an output path, we can submit real frames and verify decoded video.

---

## 18. PHASE 3c — Real frame load + desc-PA register hunt (partial)

Probe module: `afterburner_frame.ko`. Two goals: (A) pin the desc-PA register by sweeping candidate
offsets, (B) load a real ProRes frame into DMA memory and submit it.

### Result 1 — NO BAR register accepts the descriptor PA ⚠️⚠️

Swept 24 candidate offsets (§7.4's `+0x4080`/`+0x5080` family, absolute BAR0 interpretations,
counter section, engine bank 0). **Every single one ignored the write** — read back their own
pre-existing values, not our PA. This is a critical finding:

- The card does NOT take the descriptor physical address via a simple `writel(pa, bar0+offset)`.
- §7.4's `desc_lo`/`desc_hi` registers (`+0x4080`/`+0x5080`) are either at different offsets than
  documented, or the addressing mechanism is fundamentally different (e.g., the ring location is set
  during init and fixed thereafter, not per-submit).

**Implication:** The kext likely sets up the descriptor ring location ONCE during initialization
(`init`/`start` sequence) at a fixed address, and subsequent submits just fill descriptors in that
pre-registered ring + ring the doorbell. We haven't replicated the init sequence yet, so the card
doesn't know where our ring is. This is why §17's submit still "worked" (the card reacted to the
doorbell) but didn't actually process our descriptor — it had no valid ring location registered.

### Result 2 — 8 MB coherent DMA allocation failed ⚠️

`dma_alloc_coherent(8MB)` returned NULL. The system has an IOMMU and plenty of Normal-zone RAM,
but the contiguous coherent pool is too small for an 8 MB single allocation. Need to either:
- Use non-coherent `dma_alloc()` + explicit `dma_sync_single_for_device()`, OR
- Reduce frame size (submit a smaller crop), OR
- Allocate in chunks and chain descriptors.

### Phase 3c verdict

⚠️ **Desc-PA register NOT found** — no BAR offset accepts the PA write. The addressing mechanism is
init-time, not per-submit. ⚠️ **8 MB coherent alloc failed** — need non-coherent DMA or smaller frame.

**Key insight for Phase 3d:** We must replicate the kext's INIT sequence (which registers the ring
location with the card) before submits will actually process descriptors. The doorbell works (§17),
but without a registered ring, the card has no valid ring to fetch descriptors from. Next step: reverse
the init/start register writes from the kext and replay them in our driver before submitting.

---

## 19. PHASE 3d — BAR1 discovery + IODMACommand revelation (major correction)

### Finding 1 — The card has TWO memory BARs; we only mapped one ⚠️⚠️⚠️

`lspci -vvs 0f:00.0` confirms:
- **BAR0**: `74c00000`, size **128K** (what all our Phase 0–3c work mapped)
- **BAR1**: `74c20000`, size **64K** ← **the REAL DMA register space — we never mapped it!**

The kext's `start()` disassembly shows it maps BOTH:
```
this+0x128 = mapInto(BAR0)   → writeBAR0Register / readBAR0Register
this+0x130 = mapInto(BAR1)   → writeDMARegister / readDMARegister
```

**This is why Phase 3c's desc-PA sweep found nothing:** we were sweeping BAR0 offsets, but the DMA
registers (doorbells, desc_lo/desc_hi) live in **BAR1**. Every "ignored write" was actually a write to
the wrong physical location.

### Finding 2 — BAR1 is alive and has a rich structure ✅

`afterburner_bar1.ko` mapped BAR1 and dumped it (see `linux-driver/bar1_dump.txt`). Layout:
- **`+0x0`–`+0xff`**: header block (`bar1+0x0 = 0x1fc08006`, `bar1+0x4c = 0x00010140`)
- **`+0x1000`**: channel 0 base (`0x1fc18006`)
- **`+0x2000`–`+0x2f00`**: channels 1–15 (stride 0x100, base `0x1fc2XXXX`, status `0x00000001`)
- **`+0x3000`–`+0x33xx`**: engine blocks with config (`0x1234` magic, `0x55`, `0x00010009`)

The §7.4 offsets (`+0x4`, `+0x1004`, `+0x4080`) read **zero** against BAR1 — those were the kext's
*internal struct* offsets, not raw BAR1 addresses. The real doorbell/desc registers are at different
BAR1 offsets that we haven't pinned yet.

### Finding 3 — The kext uses IODMACommand (IOKit DMA framework), NOT raw register writes ⚠️⚠️

Disassembly of `getDescriptorList`/`submitFrame` shows the kext drives DMA through **IODMACommand**
vtable calls (`[rax+0x128]`, `[rax+0x130]`, `[rax+0x1c8]`, etc.) — Apple's IOKit DMA framework. The
kext does NOT write BAR1 doorbell/desc registers directly; **IODMACommand handles the ring setup and
doorbell internally** (in kernel space, via the `writeDMARegister` accessor).

**Implication for Linux:** We don't have IOKit/IODMACommand. We must replicate what IODMACommand does:
1. Allocate a descriptor ring in DMA-coherent memory
2. Write the ring's physical address to the card (via BAR1 — but WHICH offset? That's still unknown)
3. Fill descriptors into the ring
4. Ring the doorbell (BAR1 write — which offset?)

The missing piece is still: **which BAR1 offsets hold the desc-ring PA and the doorbell?** The kext
hides this inside IODMACommand's internals. We need to either:
- Trace `writeDMARegister` call sites with their offset arguments (the compiler inlined them, so we
  must find the `mov [dma_base + OFFSET], value` patterns), OR
- Empirically sweep BAR1 offsets for a writable desc-PA register (now that we're looking at the RIGHT BAR).

### Phase 3d verdict

✅ **BAR1 discovered and mapped** — it's alive, has channel/engine structure. ⚠️ **§7.4 offsets were
wrong** (they were kext-internal, not raw BAR1). ⚠️ **IODMACommand hides the real register offsets** —
we need to trace `writeDMARegister` inlined writes or empirically sweep BAR1 for the desc-PA + doorbell.

**Next: Phase 3e — pin the real BAR1 doorbell + desc-PA offsets.** Two approaches: (A) trace all
inlined `mov [dma_base+off], val` patterns in the disassembly to extract exact offsets, (B) empirically
sweep BAR1 with a test PA write and read-back. Approach A is cleaner if the compiler left the offsets
as immediates.

### Finding 4 — configureInterrupts() reveals the real BAR1 register layout ✅✅

Disassembled `configureInterrupts()` at `0x2138`. It writes these exact BAR1 registers:

| Offset | Value | Meaning (inferred) |
|--------|-------|-------------------|
| **BAR1+0x90** | `0xfffffe` | Interrupt mask/enable (all except bit 0) |
| **BAR1+0x1090** | `0xfffffe` | Second interrupt reg (channel 1?) |
| **BAR1+0x2010** | `3` | Interrupt config = 3 |
| **BAR1+0x2004** | `0x16` | **DOORBELL** — written at end of init! |

It also writes to `[this+0x140]` and `[this+0x148]` regions (set up by IODMACommand, not direct BARs):
- `[rcx+0x30]=0x7d`, `+0x34=0x7d`, `+0x40..+0x68`=various, `+0x54=0xbcd3ba4d` (MAGIC), `+0x6c=0xbf58bcd3` (MAGIC)
- These magics match BAR0's decoder section header — the descriptor ring config lives in a mapped region that mirrors the decoder layout.

**The doorbell is at BAR1+0x2004, NOT +0x4 or +0x1004 as §7.4 claimed.** The init writes `0x16` to it; per-submit likely writes a different value (frame index? command type?).

### Phase 3d final verdict

✅ **BAR1 discovered** — real DMA register space, alive with channel/engine structure.
✅ **Doorbell located: BAR1+0x2004** (from `configureInterrupts` disassembly).
✅ **Interrupt regs: BAR1+0x90, +0x1090, +0x2010.**
⚠️ **Desc-ring PA register still unknown** — it's in the `[this+0x140]`/`[this+0x148]` regions set up by IODMACommand, not a simple BAR offset. Need to trace where IODMACommand writes the ring PA.

**Next: Phase 3e — (A) verify doorbell at BAR1+0x2004 with a test write + read-back, (B) find the desc-ring PA register by tracing IODMACommand's setup or sweeping BAR1 for a writable PA slot.**

### Finding 5 — Phase 3e results: doorbell is a LATCH, not self-clearing; no PA reg in first 4KB ⚠️

`afterburner_bar1_test.ko` ran both tests (see `linux-driver/bar1_test.txt`):

**Part A — Doorbell at BAR1+0x2004:**
- Before: `0x00000000`
- After write(0x16): reads back `0x00000016` — **LATCHED, not self-clearing**
- After 50ms wait: still `0x00000016`
- No change in channel status (`+0x104c`) or engine blocks (`+0x3000`)

**Interpretation:** BAR1+0x2004 is a **command latch**, not a W1C doorbell. The card likely:
- Reads the latched value asynchronously (we need to check a *different* status/ack register)
- OR requires a separate trigger write elsewhere to "fire" the latched command
- OR the value `0x16` has specific bit meaning (bit 4 = enable? bits 0–3 = channel?)

**Part B — Desc-PA sweep (first 4KB of BAR1):**
- No offset accepts our PA write (read-back never matches)
- Broad sweep every 0x20 bytes from 0x0 to 0xfff: zero matches, zero partial changes

**Interpretation:** The desc-ring PA is NOT at a simple BAR1 offset in the first 4KB. It's either:
1. In system RAM (written by IODMACommand to a mapped region that's not a direct BAR)
2. At a higher BAR1 offset (> 0x1000) — need to sweep the full 64KB
3. Set once during init via a different mechanism (SPI flash? config register in BAR0?)

### Phase 3e verdict

✅ **Doorbell confirmed at BAR1+0x2004** — it's a latch, not self-clearing. Need to find the ack/status reg or trigger mechanism.
⚠️ **Desc-PA NOT found in first 4KB of BAR1.** Next: sweep full 64KB + investigate IODMACommand's ring setup path.

### Finding 6 — DESC-RING PA REGISTER FOUND: BAR1+0x3080/0x3084 ✅✅✅

Full 64KB BAR1 sweep (every 0x20 bytes) found **one exact PA match**:

```
*** FULL MATCH at bar1+0x3080: PA ACCEPTED ***
```

Our DMA buffer's physical address (`0x3d050000`) was written to `bar1+0x3080` (lo) and `bar1+0x3084` (hi), and **read back correctly**. This is the **descriptor-ring base address register**!

**Additional findings from the sweep:**
- Channel blocks (`+0x2000`–`+0x2f00`): writing to channel base affects status at `base+0x80` (cleared from `0x16` → `0x0`) and sets a descriptor pointer at `base+0x180` (mirrors the PA, with bit 30 cleared: `0x3d050000` → `0x1d050000`)
- Engine block `+0x3040`: changed from `0x55/0x55` → `0x0/0x0` when we wrote to nearby `+0x3080`
- Doorbell neighborhood (`+0x2000..+0x2040`): no side effects on doorbell write — it's a pure latch

**The DMA register map is now:**
| Offset | Purpose |
|--------|---------|
| **BAR1+0x3080/0x3084** | **Descriptor ring base PA (lo/hi)** ✅ CONFIRMED |
| BAR1+0x2004 | Doorbell/command latch |
| BAR1+0x90, +0x1090 | Interrupt mask regs |
| BAR1+0x2010 | Interrupt config |
| BAR1+0x2000–0x2f00 | Channel blocks (stride 0x100), each with status at `base+0x80`, desc ptr at `base+0x180` |
| BAR1+0x3000–0x3fff | Engine blocks (stride 0x100) |

### Phase 3e final verdict

✅ **DESC-RING PA REGISTER FOUND: BAR1+0x3080/0x3084** — the card accepts a physical address here and reads it back.
✅ **Doorbell at BAR1+0x2004** confirmed (latch, not self-clearing).
⚠️ **No visible ack/status change on doorbell write** — need to determine how the card signals completion (IRQ? polling a different reg?).

**Next: Phase 3f — first real DMA submit.** Now that we know where to point the ring (BAR1+0x3080) and where to ring the doorbell (BAR1+0x2004), we can:
1. Allocate a descriptor ring in coherent DMA memory
2. Fill it with a valid ProRes decode descriptor (pointing to our frame data)
3. Write the ring's PA to BAR1+0x3080/0x3084
4. Ring the doorbell at BAR1+0x2004
5. Check for completion (IRQ or polling)

This is the moment of truth — will the card actually fetch our descriptor and start decoding?

### Finding 7 — Phase 3f: Card does NOT fetch ring with bare PA + doorbell ⚠️

`afterburner_dma_test.ko` tested the minimal submit path:
1. Allocated 4KB coherent DMA buffer (filled with `0xA5`)
2. Wrote its PA to BAR1+0x3080/0x3084 ✅ (read back correctly)
3. Ringed doorbell at BAR1+0x2004 (wrote `0x16`)
4. Waited 500ms

**Result: NO DMA activity.** The card did not read from or write to our buffer, and no status registers changed.

**Interpretation:** Writing a PA to BAR1+0x3080 + ringing the doorbell is **not sufficient** to trigger a DMA fetch. The card requires additional setup before it will act on the descriptor ring:
- The ring must contain valid descriptors (not just raw memory)
- A channel/engine must be selected and configured
- The IODMACommand framework does more than PA + doorbell — it sets up segment lists, transfer parameters, etc.

**What we've confirmed:**
- BAR1+0x3080 is a valid register (accepts PA writes) but not the only thing needed
- The card is passive until properly configured through the full IODMACommand sequence
- Doorbell value `0x16` alone doesn't trigger DMA without proper channel/engine setup

### Phase 3f verdict

⚠️ **Bare PA + doorbell does NOT trigger DMA.** Need to replicate more of the kext's init/submit path:
1. Reverse the descriptor format from `getDescriptorList` (understand `Segment64` structure)
2. Replicate channel/engine configuration that happens before submit
3. Try different doorbell values to see if they select different channels/engines

**Next: Phase 3g — Deep reverse of descriptor construction + channel config.** This requires careful disassembly analysis of `getDescriptorList`, `submitDataGated`, and the IODMACommand framework calls to understand what a valid submit looks like.

### Finding 8 — Phase 3g: Kext init sequence replicated, but still no DMA ⚠️

`afterburner_init_seq.ko` replicated the exact init sequence from disassembly (0x213c-0x21ba):
1. Wrote interrupt masks to BAR1+0x90 and BAR1+0x1090 (value `0xfffffe`) ✅
2. Wrote interrupt config to BAR1+0x2010 (value `0x3`) ✅
3. Filled desc ring buffer with kext's exact pattern (magic values "MBAK" and "DCKB") ✅
4. Filled secondary config buffer (values at +0x2c, +0x30, +0x34) ✅
5. Wrote desc PA to BAR1+0x3080/0x3084 ✅ (read back correctly)
6. Ringed doorbell at BAR1+0x2004 (value `0x16`) ✅

**Result: NO DMA activity.** The card did not read from or modify our desc buffer, and no status registers changed.

**Interpretation:** The disassembly we found is **not the complete init path**. It's likely a *partial* or *secondary* initialization routine. The main init sequence (which sets up the IODMACommand framework, allocates the actual descriptor ring in system RAM via IOKit's DMA allocator, and configures the channel/engine) happens elsewhere — inside the IODMACommand implementation, which we don't have disassembly for.

**Key insight:** The kext uses **IODMACommand** (IOKit's DMA command framework), which:
1. Allocates a descriptor ring in system RAM using IOKit's DMA allocator (not raw `dma_alloc_coherent`)
2. Builds a linked list of `Segment64` structures describing the transfer
3. Calls internal IODMACommand methods that write to hardware registers

The actual register writes happen inside the IODMACommand implementation, which is part of macOS's IOKit framework (not the kext). We don't have access to its disassembly.

### Phase 3g verdict

⚠️ **Replicating the visible init sequence is not sufficient.** The card requires full IODMACommand framework initialization, which we cannot replicate without reverse-engineering macOS's IOKit DMA framework.

**Next options:**
1. Find IODMACommand source code (if Apple has released it) or disassemble macOS's IOKit framework
2. Try different register combinations empirically until something works
3. Accept that full ProRes decode may require running on macOS (where the kext + IOKit work together), and focus our Linux driver on other features (flash management, status monitoring, etc.)

### Finding 8 CORRECTION — Phase 3g had three errors (resolved by arm64e deep-dive, §12)

The arm64e disassembly of the real submit path (~0x68bc–0x69c8, confirmed in both
slices) shows that all three assumptions behind the Phase 3f/3g experiments were wrong:

**Error 1 — Wrong doorbell register and value.**
Phase 3f/3g rang `BAR1+0x2004 = 0x16` expecting a DMA fetch. That write is part of
the *init* sequence (arm64e @0x5a3c, x86_64 equivalent at 0x213c–0x21ba) — it
configures the engine, not a doorbell. The actual DMA GO writes are:

| Mode | Clear | Desc words | GO write |
|------|-------|-----------|----------|
| single (`mode==0`) | `BAR1+0x0004 = 0` | `+0x4080`=lo, `+0x4084`=hi, `+0x4088`=ctrl | **`BAR1+0x0004 = 0xFFE7F`** |
| batch (`mode==1`) | `BAR1+0x1004 = 0` | `+0x5080`=lo, `+0x5084`=hi, `+0x5088`=ctrl | **`BAR1+0x1004 = 0x80FFE7F`** |

Confirmed in x86_64 (§7.4) and arm64e (mov w8,#0xfe7f; movk w8,#0xf,lsl#16 →
0xFFE7F at ~0x68e0–0x68e8; batch variant 0x80FFE7F at ~0x699c–0x69a4).

**Error 2 — Wrong descriptor-pointer register.**
Phase 3e/3f wrote the ring's physical address to `BAR1+0x3080/0x3084` (found by a
sweep that reported "PA accepted"). The kext **never writes to +0x3080** — no
instruction in either slice builds that offset from the BAR1 base. The per-submit
descriptor address is written as a *value* into `+0x4080/+0x4084` (lo/hi) and
`+0x4088` (control), or `+0x5080/+0x5084/+0x5088` for batch. The "PA acceptance"
at +0x3080 was a red herring: that register simply echoes back writes (scratch or
status inside an engine block).

**Error 3 — Wrong conclusion about IODMACommand.**
Phase 3g concluded the actual register writes happen "inside the IODMACommand
implementation, which is part of macOS's IOKit framework" and declared full
replication impossible without reverse-engineering IOKit. Both slices show all
hardware register writes are **in the kext itself** (`submitDataGated` at
~0x97f0–0x9bc0 arm64e / 0x4d98 x86_64; submit path ~0x68a4–0x69c8). The kext uses
IODMACommand only for coherent memory allocation and physical-address retrieval —
it does not delegate DMA register programming to IOKit. A Linux driver can
replicate the full submit path with `dma_alloc_coherent` + the MMIO writes above;
no IOKit reverse-engineering is required.

**Corrected Phase 3g verdict:** The init sequence was replicated correctly (masks,
config, buffer fills all match arm64e @0x5a3c). What failed was the *submit* step:
wrong doorbell register/value and wrong desc-pointer register. With the corrected
sequence from §12.1/§7.4, a Linux driver can issue real DMA submits without any
IOKit dependency.

---

## 20. PHASE 5 — Bitfile load path FULLY DECODED (disasm + live BAR dump)

The complete answer to "how does the kext point at bitfiles, pick a version, and program them on load".
Every claim below is verified against `rework/x86_64.bin` disassembly AND the idle-state BAR dump in
`linux-driver/bar_map.txt`.

### 20.1 The BAR-space descriptor chain (how start() finds every block)

BAR0+0x0 holds a **statically-present** tagged descriptor chain (verified: it is fully populated in the
idle Linux dump — no init needed, FPGA firmware ships it). Each entry is one 32-bit word:

```
valid if low byte != 0;   tag = (value - 1) & 7;   stride to next entry = 4 * (value >> 16) bytes
```

Walked from BAR0+0x0 on the live card (all values read from bar_map.txt):

| BAR0 offset | word        | tag | driver slot      | block content (idle value)                          |
|-------------|-------------|-----|------------------|-----------------------------------------------------|
| +0x0000     | 0x04000102  | 1   | self->+0x138 STATUS   | hdr, board_rev@+4=0x10020014, img_rev@+8=0x0002003E, fuse_user@+0xC=0xEC01003F |
| +0x1000     | 0x04000103  | 2   | self->+0x140 (main)   | Phase-0 "decoder section": magic hdr @+0x1040, desc table @+0x1080 |
| +0x2000     | 0x04000104  | 3   | self->+0x148 CONTROL  | reconfig command @+4 (idle: 0x00000100)             |
| +0x3000     | 0x04000107  | 6   | self->+0x150 FLASH    | SPI cmd block: status@+0x10 (idle 0x1F), addr@+0x40, cmd@+0x48, data@+0x4C, done@+0x54 |
| +0x4000     | 0x10000106  | 5   | (skip)              | stride 16KB — jumps over unused +0x4000..+0x7FFF    |
| +0x8000     | 0x000b0101  | 0   | self->+0x158 DECODER  | first of 8 channel blocks (stride 0x1000); enable @+4 (idle: 0) |
| +0x802C     | 0           | —   | terminator          |                                                     |

`start()` walks this chain via the tag jump table at binary offset `0x1e68`; each case stores the block
base into its slot and sets a "found" flag (tag 0 → `[self+0x11d0]=1`). Slots are optional —
`handleInterrupt` NULL-checks the decoder-control block. BAR1 has only 2 non-zero words in idle state.

### 20.2 start() on our card: no flash activity at load time

`start()` (board class = `(board_rev>>16)&0xF`, jump table @0x1e84):
- **class 2** (our card, board_rev=0x10020014) → logs `"image type is live"` (str@0x84c3), then requires:
  `[self+0x164]!=0` (board_rev&0xF), `[self+0x168]!=0` (set in `init()` @0x6b7c), decoder found
  (`[self+0x11d0]`), image major==2 and `(minor & 0xFFFE)==0x3E` — i.e. **running image is 2.62 or 2.63**.
- Our card (img_rev=0x0002003E = 2.62) passes → logs str@0x860e:
  `"decoder found on card. live image revision 2.62 matches driver flash image... skipping flash update"`
  → start() succeeds. **The kext does NOT touch the bitfiles at load time when the running image is current.**

### 20.3 reconfig_handler state machine (the actual trigger)

`reconfig_handler(IOInterruptEventSource*, int)` @0x2e66 — fires on FPGA interrupt:
1. If `[self+0x170]` (update-in-progress flag) set: flush loop over dwords `self->+0x174..+0x1173`
   calling a method on the `[self+0x88]` object per dword, then clear flag.
2. Log board revision (str@0x8331) and image revision (str@0x836c).
3. Switch on `(board_rev>>16)&0xF` — this word doubles as the **booted image type**:

| class | log string            | behavior |
|-------|-----------------------|----------|
| 0     | "image type is golden" (0x8fd5) | if img_rev > 0x1FFFF or low16(img) > 0x18 → **flash reprogram path** (§20.4); else `"unsupported golden image... bailing out"` (0x83af) |
| 1     | "image type is HWTE..." (0x91ea) | log + return |
| 2     | (live boot — our card) | if img ∈ {2.62, 2.63} or `[self+0x16c]`>1 → up-to-date path: log `"image update is complete!"` (0x9287), wait 1s on [self+0x1188], **CONTROL+4 = 0x19000** (reconfigure to golden); else log `"image type is live... reconfiguring to golden for flash update"` (0x9224), set `[self+0x170]=1`, wait on [self+0x1178], **CONTROL+4 = 0x19000** |
| 3     | "image type is debug..." (0x9373) | log + return |
| >3    | "image type is unknown..." (0x93ae) | error, return |

So the update is **two-phase across two interrupts**: a stale live card first gets switched to golden
(CONTROL+4=0x19000); on the *next* interrupt the status block reads class 0 (golden), which runs the
actual flash reprogram and ends with CONTROL+4 = **0x1A000** (reconfigure to live — boots the new image).

`pointer_restore_handler()` @0x3646 is a second interrupt handler for reset events: same +0x170 flush,
then `"image type is live... reconfiguring to golden for pointer restore"` (0x92c5) → CONTROL+4=0x19000.

### 20.4 The flash reprogram sequence (class-0 path @0x31ca/0x3294)

```
attempts = [self+0x16c]                      ; init() zeroes it; max 2 attempts
if attempts >= 2: log "giving up on flash update. max attempts" (0x900e); return
log fuse user word (0x9060)                   ; from STATUS+0xC
ec = (fuse >> 16) & 0xFF                      ; EC level byte
if ec >= 2: log "unknown version %u. giving up" (0x9096); return
family = fuse & 3                             ; low 2 bits select file family
if [self+0x1198] == NULL: loadBitfile(name_for_slot1, slot=1)   ; lazy — first update only
if [self+0x11a0] == NULL: loadBitfile(name_for_slot2, slot=2)
inc [self+0x16c]
log "changing pointer to golden..." (0x916b); change_pointer(0)   ; write golden pointer table to flash
wait 1s; spierase(addr = 0x1400000)           ; erase bitfile region in SPI flash
spiprogram(slot=0, addr=0x1400000, data=[self+0x1198]->data, len, flag=1)   ; primary bitfile
spiprogram(slot=1, addr=0x1400000, data=[self+0x11a0]->data, len, flag=1)   ; secondary bitfile
[self+0x170] = 1; wait 1s on [self+0x1188]
CONTROL+4 = 0x1A000                           ; reconfigure to LIVE → FPGA boots newly programmed image
```

- SPI command block = FLASH slot (BAR0+0x3000): status@+0x10, address@+0x40, command@+0x48, data@+0x4C,
  completion flag@+0x54. `spierase(u32 addr)` / `spiprogram(u32 slot, u32 addr, void* data, u64 len, bool)`
  drive it with bank selection + endianness handling (see §9).
- Bitfiles are programmed at flash address **0x1400000** (20 MB — second bank region); both primary and
  secondary go to the same address (slot arg selects which pointer table / copy).
- `change_pointer(u32)` @0x4270 writes the kext's embedded pointer tables (live/golden, 64-byte records,
  magic 0xA9560000, at binary offsets 0xaf00/0xaf40/af80/afc0) to flash — these tell the FPGA which image
  region to boot.

### 20.5 Bitfile selection (exact rules + templates)

Files live in `AppleAfterburner.kext/Contents/Resources/` (genuine AMD/Xilinx images, sync word
0x442211BB @+0x48). Filenames are built at runtime with snprintf:

| condition        | slot 1 (primary)                    | slot 2 (secondary)                  |
|------------------|-------------------------------------|-------------------------------------|
| `fuse & 3 == 3`  | `live_2.62_0x01_primary_ec%02X.bin` | `live_2.62_0x01_secondary_ec%02X.bin` |
| else             | `live_2.63_0x01_primary.bin`        | `live_2.63_0x01_secondary.bin`      |

- `%u.%u` = hardcoded target version (2, 0x3E for the EC family; 2, 0x3F otherwise); `_0x01_` is a
  **constant in the format string**, not fuse-derived.
- `ec%02X` = `(fuse_user >> 16) & 0xFF`.
- Our card: fuse=0xEC01003F → family=3, ec=0x01 → would load `live_2.62_0x01_primary_ec01.bin` +
  `..._secondary_ec01.bin` (both exist in the bundle).

### 20.6 loadBitfile() mechanics @0x42f2

`loadBitfile(const char* name, u32 slot)`: gets the kext identifier via `_OSKextGetCurrentIdentifier`,
creates an **IOFileLoader** over `Contents/Resources/<name>` with custom callback
`resourceLoadCallback` (binary 0x43fa). When IOKit needs file data it calls back →
`resourceLoadCallbackHandler()` → tail call to `submitResource(IOBufferMemoryDescriptor*, u32)` — the same
IODMACommand machinery as frame submit, dispatched through `submitResourceGated` @0x4c70. Resulting buffer
descriptor is stored in `[self+0x1198]` (slot 1) / `[self+0x11a0]` (slot 2); both slots are NULL-initialized
in `init()` @0x6b7c along with flags +0x168/+0x16c.

### 20.7 Our card's state, summarized

- Board class **2** (live boot), running live image **2.62** (BAR0+0x8 = 0x0002003E) — matches the driver's
  bundled images → start() skips flash update; reconfig_handler would log "image update is complete!".
- Fuse user word 0xEC01003F: family=3 (EC-capable), EC level 0x01.
- Idle registers: CONTROL+4 = 0x100, FLASH status = 0x1F, decoder enable = 0 (disabled from Linux).
- A flash update on this card would only run if the running image were not 2.62/2.63 — it would program
  the ec01 2.62 pair at flash address 0x1400000 and switch to live via CONTROL+4=0x1A000.

**Phase 5 status: COMPLETE for the kext-side path.** Remaining unknowns are FPGA-internal (what exactly
the golden marker image reports in STATUS, and the full SPI command encoding — see §9).

---

## 1. The big correction (this session)

Earlier passes chased a long run of numbered accessors `readBAR<n>Register` /
`writeBAR<n>Register` for **n = 1379 … 1700+**. Every one of those symbols is an
**empty stub** — the disassembler matches the symbol header but emits no instruction
body. They carry no logic and are not worth documenting individually.

The binary actually contains only **seven real register-accessor families**, each a
thin pointer-deref wrapper over a per-BAR memory-mapped region:

| Family | read offset (self) | write offset (self) | Null-guarded? |
|---|---|---|---|
| `BAR0`     | `+0x128` | `+0x128` | no  |
| `DMA`      | `+0x130` | `+0x130` | no  |
| `Version`  | `+0x138` | `+0x138` | no  |
| `Thermal`  | `+0x140` | `+0x140` | no  |
| `Bitfile`  | `+0x148` | `+0x148` | no  |
| `Flash`    | `+0x150` | `+0x150` | no  |
| `Decode`   | `+0x158` | `+0x158` | **yes** (returns 0 / no-op if NULL) |

So the "BAR number" in the old naming was a red herring — there is one BAR0 window
plus six named sub-regions, all at fixed offsets inside the driver object.

---

## 2. Register accessor implementations (verbatim disassembly)

All follow the same shape: load the region pointer from `self+offset`, then do a
32-bit read/write at byte offset `%esi` (the register index). Writes take value in
`%edx`.

```asm
; ---- BAR0 (+0x128) ----
readBAR0RegisterEj:            ; 0x1e94
    pushq %rbp
    movq  %rsp, %rbp
    movq  0x128(%rdi), %rax     ; rax = self->bar0_ptr
    movl  %esi, %ecx
    movl  (%rax,%rcx), %eax     ; return *(u32*)(bar0 + off)
    popq  %rbp
    retq

writeBAR0RegisterEjj:          ; 0x3e12
    pushq %rbp
    movq  %rsp, %rbp
    movq  0x128(%rdi), %rax
    movl  %esi, %ecx
    movl  %edx, (%rax,%rcx)     ; *(u32*)(bar0 + off) = val
    popq  %rbp
    retq

; ---- DMA (+0x130) ----
readDMARegisterEj:             ; 0x3e36   (same body, offset 0x130)
writeDMARegisterEjj:           ; 0x3e24

; ---- Version (+0x138) ----
readVersionRegisterEj:         ; 0x1ea6   (offset 0x138)
writeVersionRegisterEjj:       ; 0x3e48

; ---- Thermal (+0x140) ----
readThermalRegisterEj:         ; 0x3e5a   (offset 0x140)
writeThermalRegisterEjj:       ; 0x3e6c

; ---- Bitfile (+0x148) ----
readBitfileRegisterEj:         ; 0x3e7e   (offset 0x148)
writeBitfileRegisterEjj:       ; 0x3e90

; ---- Flash (+0x150) ----
readFlashRegisterEj:           ; 0x3ea2   (offset 0x150)
writeFlashRegisterEjj:         ; 0x3eb4

; ---- Decode (+0x158) — the ONLY null-guarded pair ----
readDecodeRegisterEj:          ; 0x21c6
    pushq %rbp
    movq  %rsp, %rbp
    movq  0x158(%rdi), %rax     ; rax = self->decode_ptr
    testq %rax, %rax
    je    .Lnull                ; if NULL -> return 0
    movl  %esi, %ecx
    movl  (%rax,%rcx), %eax
    jmp   .Lend
.Lnull:
    xorl  %eax, %eax            ; return 0
.Lend:
    popq  %rbp
    retq

writeDecodeRegisterEjj:        ; 0x3dfa
    pushq %rbp
    movq  %rsp, %rbp
    movq  0x158(%rdi), %rax
    testq %rax, %rax
    je    .Lend                 ; if NULL -> no-op (skip write)
    movl  %esi, %ecx
    movl  %edx, (%rax,%rcx)
.Lend:
    popq  %rbp
    retq
```

**Interpretation.** `Decode` is the only region that may be absent at runtime (the
driver tolerates a NULL decode window), hence the guard. The other six are assumed
always mapped once `start()` succeeds.

---

## 3. Object layout — where the pointers live

From `init(OSDictionary*)` and `start(IOService*)`, the driver object is large
(~0x1310 bytes). Relevant fields:

| Offset | Meaning (inferred) |
|---|---|
| `+0x88`  | parent / provider IOService handle (used to fetch BARs in `probe`) |
| `+0x90`  | first mapped resource object (`start`: `callq *0x140(%rcx)` then stored) |
| `+0x98`  | second resource object; source of the BAR windows |
| `+0xa0`  | third resource (mapped, null-checked at `0x1337`) |
| `+0xa8`  | fourth resource (mapped, null-checked at `0x1355`) |
| `+0xb0`  | fallback pointer; if the primary fetch fails, copied from global `0xb018` |
| `+0x118` | object obtained via vtable slot `*0x908(%rax)` with arg `0x10` (BAR index 16?) |
| `+0x128` | **bar0_ptr** — BAR0 window base (`read/writeBAR0Register`) |
| `+0x130` | **dma_ptr**    — DMA region base |
| `+0x138` | **version_ptr**— Version region base |
| `+0x140` | **thermal_ptr**— Thermal region base |
| `+0x148` | **bitfile_ptr**— Bitfile region base |
| `+0x150` | **flash_ptr**  — Flash region base |
| `+0x158` | **decode_ptr** — Decode region base (nullable) |
| `+0x164` | low nibble of a version/ID word (`andl $0xf`) stored during start |
| `+0x1270`| initialized to `-1` in `init` (sentinel / index) |

In `start()`, the six non-BAR0 pointers are **zeroed first** (`movq %rbx, 0x158/0x150/
0x148/0x140/0x138(%r12)` with `%rbx=0`), then a dispatch table at `0x1e68` (7 entries,
indexed by `(reg-1)`, range-checked against 6) assigns each region pointer from the
mapped BAR. The BAR0 window (`+0x128`) is populated earlier via the vtable call chain
(`*0x908(%rax)` → `*0x118(%rcx)`).

---

## 4. Lifecycle functions (captured)

### `init(OSDictionary*)` — `0x6b7c`
- Calls superclass init via global vtable at `0xb030`, slot `*0x258`.
- On success: stores the returned object into several fields (`+0x90, +0x98, +0xb0`)
  and zero-fills a block of scalar/pointer fields.
- Sets `+0x1270 = -1` (sentinel).
- Zeroes an 8-element byte array at `+0x11e4`.
- Returns 1 on success, 0 otherwise.

### `probe(IOService*, int*)` — `0x6c98`
- Fetches a candidate via vtable slot `*0x5c8(%rax)` (global `0xb030`).
- If non-null: stores it at `+0x88` and returns success.
- Else: logs a failure string (`0xaa21`/`0xaa51`) through the logger vtable slot
  `*0x388(%rax)` and returns 0.

### `start(IOService*)` — `0x12a6` (the meat)
Sequence, in order:
1. Superclass start via global `0xb030`, slot `*0x5d0`; bail if false.
2. Map resource objects into `+0x98`, `+0x90`, `+0xa0`, `+0xa8` (each null-checked;
   failure logs and aborts).
3. Initialize a 16-byte linked structure spanning `+0x11f0 … +0x1270`.
4. Resolve the BAR source: primary fetch into `+0xb0`; on NULL, fall back to global
   at `0xb018` (still null-checked).
5. Obtain two more objects via vtable slot `*0x908(%rax)` with args `0x10` and `0x14`,
   stored at `+0x118` / `+0x120`; then the BAR0 window into `+0x128`.
6. **Zero all six named region pointers** (`+0x138…+0x158`).
7. Dispatch-table loop (table at `0x1e68`, 7 entries) assigns each region pointer from
   the mapped BAR, range-checking the index against 6.
8. If a "decode present" flag (`+0x11d0`) is clear and decode ptr is NULL → log + abort.
9. Read a version/ID word: `movl 4(%r10), %ebx` (from version region), store low nibble
   at `+0x164`, log the full value plus two derived fields (`shrl $0x10`).
10. Call **`updateIORegistryProperties()`** (`0x1eb8`) — publishes device info to IOKit.
11. Call **`configureInterrupts()`** (`0x2138`) — sets up the interrupt event source.
12. Second dispatch table at `0x1e84` (4 entries, index = `(word>>16)&0xf`, range ≤ 3)
    selects a per-variant init path; one branch checks two bounds (`≤ 0x1ffff` and
    `≤ 0x18`) before logging.

---

## 5. Other notable symbols (not yet disassembled — next up)

Functional / DMA-path:
- `submitData`, `submitFrame`, `submitResource`, `submitDataGated`, `submitResourceGated`
- `queryCompletion`, `queryCompletionGated`
- `getSegments`, `getResourceSegments`, `getDescriptorList`, `getResourceDescriptorList`
- `bindFrame`, `unbindFrame`, `checkForSpace`, `freeSpace`

Firmware / SPI:
- `spiprogram(u32, u32, void*, bool, bool)`, `spierase(u32)`
- `loadBitfile(const char*, u32)`, `resourceLoadCallback`, `resourceLoadCallbackHandler`

Interrupts / timers:
- `handleInterrupt`, `reconfig_handler`, `configureInterrupts`
- `sleep_timer`, `reconfig_timer`, `pointer_restore_timer`, `pointer_restore_handler`

Power / state:
- `setPowerState`, `updateActiveTime`, `updateIdleTime`, `cardStatus`, `getPercent`
- `change_pointer`, `reset`, `stop`, `cleanUp`, `finalize`, `terminate`

UserClient (IOKit user-space client, separate class):
- `AppleAfterburnerUserClient::externalMethod` + the `s*` dispatch thunks
  (`sSubmitFrame`, `sCardStatus`, `sGetPercent`, `sQueryCompletion`, `sCheckForSpace`,
  `sBindFrame`, `sUnbindAll`) — this is the user-space API surface.

---

## 6. Open questions / next steps

1. **Decode region semantics** — why nullable? Likely an optional decode/overlay BAR
   present only on some board variants. Confirm by checking which dispatch-table entry
   (index in `0x1e68`) populates it and under what condition.
2. **The two vtable args `0x10` / `0x14`** passed to slot `*0x908(%rax)` — almost
   certainly BAR indices or resource IDs; map them to the actual PCI BARs.
3. **Version word layout** — low nibble → `+0x164`; bits 16-31 and the low 16 bits are
   logged separately. Recover the exact field meanings from the log strings at
   `0x826a`, `0x8331`, `0x836c`, `0x83af`.
4. **SPI/bitfile programming** — `spiprogram` + `loadBitfile` likely contain the flash
   write state machine worth documenting for firmware-update behavior.

---

## 7. DMA submit path (annotated) — the real register sequences

This is where the driver actually talks to hardware. Three layers:
`submitFrame` → `submitData` → `submitDataGated`, plus the descriptor builder
`getDescriptorList` and the interrupt/completion handlers.

### 7.1 Error codes (Kext-style, `0xE000xxxx`)

| Code | Meaning (inferred) |
|---|---|
| `0xE00002BC` | "busy / gated" — submit rejected while a prior op is in flight or gate held |
| `0xE00002BE` | descriptor-list build failure (alloc/map error) |
| `0xE00002D5` | "already queued" — ring slot occupied, request coalesced |
| `0xE00002D8` | generic submit failure / bad state |

### 7.2 `submitFrame(sContext*, IOMemoryDescriptor*, …)` — `0x590e`

Top-level entry (called from the UserClient `sSubmitFrame`). Flow:
1. If a "frame-in-flight" flag at `+0x12c8` is set → return `0xE00002D8`.
2. Take a lock via vtable on object at `+0x12b8` (re-checked after lock; if the flag
   flipped, bail with `0xE00002D8`).
3. If a "pending count" at `+0x160` is non-zero → log it and return `0xE00002D8`.
4. **Dump thermal/telemetry** from the Thermal region (`+0x140`): reads six 32-bit words
   at offsets `+0x10, +0x80, +0x84, +0x88, +0x8c, +0x90` (the last five shifted right
   by 7 — i.e. fixed-point values) and logs each with a distinct string (`0xa059…0xa1d3`).
   This is a per-submit thermal snapshot.
5. Release the lock; if the "decode present" flag `+0x11d0` is clear → log + return
   `0xE00002D8`.
6. Otherwise call **`submitData`** twice (two sub-frames / two descriptor sets, with a
   mode byte of 0 then 1). Any non-zero return is logged and propagated.

### 7.3 `submitData(sContext*, IOMemoryDescriptor*, u32, void*, u32,u32,u32, u64,u64)` — `0x56cc`

The workhorse. Flow:
1. Acquire a spinlock (vtable slot `*0x8c8(%rax)`, arg 1) on the provider at `+0x88`.
2. If the **Decode** region (`+0x158`) is NULL, or its status word `decode[+8]` has bit 0
   clear → log and return `0xE00002D8`. (So a live decode window is required to submit.)
3. Set `decode[+0x18] = 0x1f` (a "submitting" marker / 5-bit counter).
4. Build the DMA segment list via **`getSegments`** (returns count in `%eax`). On failure
   → log + return `0xE00002D8`.
5. Build the descriptor ring via **`getDescriptorList`**. On failure → free a 0x48-byte
   temp and return the error.
6. Allocate a 0x48-byte `sDmaConfig` (via `nw(0x48)`) and fill it:
   - `+0x10` = segment base ptr, `+0x18` = seg count, `+0x20` = user data ptr,
     `+0x1c` = mode byte, `+0x28` = ring head (`self->+0xc8`), `+0x30`/`+0x34`/`+0x38` =
     the three trailing u32 args, `+0x3c` = 1 (a "valid" flag), `+0x40` = decode ptr.
7. Submit through the **gated** path: vtable slot `*0x1c8(%rax)` on object at `+0x90`,
   passing the config and a pointer to `submitDataGated`. 
   - If it returns `0xE00002D5` ("already queued"): write the user data ptr into
     `self->+0xc8` (the ring head), and if mode==1 increment `+0xc8`; return 0.
   - Else: free the config; on non-zero, return that code; else return 0.

### 7.4 `submitDataGated(sDmaConfig*)` — `0x4d98`  ← **the hardware writes**

This is the function that actually programs registers. Key sequence (success path):

```asm
; r13 = sDmaConfig*, r14 = self, r12 = config->ring_slot_index (+0x20)

; --- gate check: if a global "in-flight" byte at +0x12f8 is set AND config->flag(+0x1c)==0
;     -> take spinlock *0x1e0 and, on failure, return 0xE00002BC (busy)

; --- ring-full check: if self->+0xc0 (ring count) >= 0x20 (32 slots):
;       build a "wait" descriptor via helper(0x7d0, 0xf4240, &out), then call vtable
;       slot *0x1f0 with arg 2 to stall/advance the ring. Loop while count > 0x1f.

; --- DECODE doorbell (only if decode region present and config->flag==0):
    movl  0xc8(%r14), %eax        ; ring head
    subl  0x1308(%r14), %eax      ; subtract base -> index
    shll  $0x10, %eax             ; << 16
    movl  0x34(%r13), %edx        ; config field A
    movl  0x38(%r13), %esi        ; config field B
    shll  $0x8,  %esi             ; B << 8
    shll  $0x4,  %edx             ; A << 4
    orl   %esi, %edx
    orl   %eax, %edx              ; (head-base)<<16 | (A<<4) | (B<<8)
    orl   0x30(%r13), %edx        ; | config field C
    movl  %edx, 0xc(%rcx)         ; *** WRITE decode[+0x0c] = doorbell value ***

; --- increment ring count: self->+0xc0 += 1 (stored back via (%r12))

; --- free-list / slot bookkeeping at +0x11f0..+0x1270 (16-byte linked slots):
;     if the slot's next-ptr != itself (i.e. it was in use):
;         config->desc(+0x10) -> desc_ring[+0x18]; clear bit0 of a flag word;
;         increment a per-slot counter at +0x3c; return 0 (success, coalesced).
;     else if the slot's "busy" byte (+0x11e4[idx]) is set:
;         splice config into the free list; return 0xE00002D5 (already queued).

; --- fresh slot path (busy byte was clear):
    movb  $0x1, 0x11e4(%r14,%rsi) ; mark slot busy
    callq updateIdleTime          ; if config->flag(+0x1c)==0
    ; rdx = config->desc_hi (+0x10 >> 32), rcx = config->desc_lo

;   *** THE TWO DMA DOORBELL VARIANTS (selected by config->flag +0x1c) ***
;   flag == 0  -> "single" doorbell:
    movq  0x130(%r14), %rax       ; rax = DMA region base (+0x130)
    movl  $0x0,     0x4(%rax)      ; dma[+0x0004] = 0          (clear status)
    movl  %ecx,     0x4080(%rax)   ; dma[+0x4080] = desc_lo
    movl  %edx,     0x4084(%rax)   ; dma[+0x4084] = desc_hi
    movl  0x18(%r13),%ecx          ; config->desc_mid
    movl  %ecx,     0x4088(%rax)   ; dma[+0x4088] = desc_mid
    movl  $0xffe7f, 0x4(%rax)      ; *** dma[+0x0004] = 0xFFE7F (GO / doorbell) ***

;   flag == 1  -> "batch" doorbell:
    movq  0x130(%r14), %rax       ; rax = DMA region base
    movl  $0x0,     0x1004(%rax)  ; dma[+0x1004] = 0           (clear status)
    movl  %ecx,     0x5080(%rax)  ; dma[+0x5080] = desc_lo
    movl  %edx,     0x5084(%rax)  ; dma[+0x5084] = desc_hi
    movl  0x18(%r13),%ecx
    movl  %ecx,     0x5088(%rax)  ; dma[+0x5088] = desc_mid
    movl  $0x80ffe7f,0x1004(%rax) ; *** dma[+0x1004] = 0x80FFE7F (GO / doorbell) ***
    movl  0x3c(%r13),%edx         ; config->valid flag
    movl  %edx,     0x12e4(%r14)  ; self->+0x12e4 = valid

;   then a final "kick" via helper(edi=0x2B610FB0, esi=self, ecx=1, r9=0)
```

**Register map (DMA region, base at `self->+0x130`):**

| Offset | Write value | Role |
|---|---|---|
| `+0x0004` | `0` then `0xFFE7F` | single-shot status clear → GO doorbell |
| `+0x4080` | desc_lo (32-bit) | descriptor low word |
| `+0x4084` | desc_hi (32-bit) | descriptor high word |
| `+0x4088` | desc_mid (32-bit) | descriptor mid / control word |
| `+0x1004` | `0` then `0x80FFE7F` | batch status clear → GO doorbell (bit 31 set = batch mode) |
| `+0x5080` | desc_lo | batch descriptor low |
| `+0x5084` | desc_hi | batch descriptor high |
| `+0x5088` | desc_mid | batch descriptor mid |

The doorbell constants `0xFFE7F` (single) and `0x80FFE7F` (batch, bit 31 = batch flag)
are the "go" tokens. The two parallel register banks (`+0x40xx` vs `+0x50xx`) let a
single-shot op and a batch op be in flight independently.

**Decode doorbell** at `decode[+0x0c]`: value = `((ring_head - base) << 16) | (A<<4) |
(B<<8) | C` — encodes the ring position plus three config fields into one 32-bit write.

### 7.5 `getDescriptorList(sContext*, IODMACommand::Segment64*, u32, void*, u32, u64)` — `0x50ee`

Builds the on-ring descriptor array:
1. Allocates a ring buffer via helper(`count<<5`, 0x1000, 3) — i.e. **32-byte descriptors**,
   page-aligned (0x1000), type 3. On NULL → log + return `0xE00002BE`.
2. Maps it for DMA: vtable slot `*0x128(%rax)` on object at `+0xa8`; if that fails, falls
   back to a manual map (helper with args 0x1000/0/0/0/1 + the provider at `+0xb0`).
3. If mapping fails → log + return `0xE00002BE`.
4. **Descriptor fill loop** (up to 0x7f = 127 descriptors, stride 0x20):
   - Each descriptor is a 32-byte struct:
     - `+0x00` = next-pointer / link (from the segment list)
     - `+0x08` = physical address of the data buffer (`movq (%r15), %rcx; movq %rcx, (%rbx)`)
     - `+0x10` = control word: `(len<<8) | 0xAD4B0000` — **tag `0xAD4B`** in the top 16 bits,
       length in bits 8-23. For the *last* descriptor (when mode==1) it ORs in `0x13`:
       control = `0xAD4B0013 | (len<<8)` and sets `+0x18 = 0` (end-of-chain marker).
     - `+0x18` = trailing qword (zeroed on the final descriptor)
   - The segment source pointer (`%r15`) advances by 0x10 each iteration; the physical
     address comes from `seg[+0]`, the length from `seg[+8]`.
5. Returns the number of descriptors written (in `%eax`), or the error code.

**Descriptor format (32 bytes):**
```
offset  size  field
+0x00   8     next / link pointer
+0x08   8     physical buffer address
+0x10   4     control: [31:16]=0xAD4B tag, [15:12]=flags(0x13 on last), [11:8..] len<<8
+0x14   4     (reserved / part of control)
+0x18   8     end-of-chain marker (0 on final descriptor)
```

### 7.6 `handleInterrupt(OSObject*, IOInterruptEventSource*, int)` — `0x21e2`

The interrupt handler, driven off the **Bitfile** region (`self->+0x148`):
1. Reads status word `bitfile[+0x14]`; if bit 0 clear → nothing to do (return).
2. Logs the full status word (string `0xa62d`).
3. Calls a completion callback: helper(`edi=0x2B610FAC`, esi=self, edx=1, ecx=status) —
   this is the "work done" notification (same helper family as the submit kick).
4. Reads two more status words `bitfile[+0x10]` and `bitfile[+0x20]`, logging each.
5. **Status decode** of `bitfile[+0x20]`:
   - writes back `(val & 0x7)` to `bitfile[+0x20]` (acknowledges the low 3 bits).
   - if bit 2 (`0x4`) set → clear `bitfile[+0x04] = 0` and log ("error/abort" path, string `0xa6fe`).
   - if bit 1 (`0x2`) set → clear `bitfile[+0x04] = 0`, log (string `0xa739`), then continue
     into the completion-drain loop (beyond the captured window).

So the interrupt path: read status → notify completion callback → ack low bits → branch on
error/normal bits. The `bitfile[+0x04]` register is a shared "clear-on-interrupt" control.

### 7.7 `queryCompletion(sContext*, void*, void*)` — `0x629e`

Polling completion (the non-interrupt path):
1. Builds a small struct on the stack with a tag of `2`.
2. Calls **`queryCompletionGated`** through vtable slot `*0x1c8(%rax)` on object at `+0x90`
   (same gated-dispatch mechanism as submit). Non-zero return is propagated directly.
3. On success, walks a completion ring: for each slot in an array indexed by 8-byte stride
   (`0x40(%r15,%r13,8)`), if the slot's next-ptr differs from itself (i.e. it holds a
   completed descriptor):
   - unlinks it from the list,
   - releases its memory object (vtable `*0x138` twice — get + release),
   - frees 0x18 bytes via `dl(0x18)`,
   - and if a "first completion" flag is set, calls two more vtable slots (`*0x1f8`, `*0x28`)
     to finalize / notify.

---

## 8. Consolidated hardware register map (DMA + Decode + Bitfile)

| Region | base offset | Register | Access | Value / meaning |
|---|---|---|---|---|
| DMA    | `+0x130` | `+0x0004` | W | `0` → `0xFFE7F` (single-shot GO) |
| DMA    | `+0x130` | `+0x4080` | W | descriptor low 32-bit |
| DMA    | `+0x130` | `+0x4084` | W | descriptor high 32-bit |
| DMA    | `+0x130` | `+0x4088` | W | descriptor mid/control |
| DMA    | `+0x130` | `+0x1004` | W | `0` → `0x80FFE7F` (batch GO, bit31=batch) |
| DMA    | `+0x130` | `+0x5080` | W | batch descriptor low |
| DMA    | `+0x130` | `+0x5084` | W | batch descriptor high |
| DMA    | `+0x130` | `+0x5088` | W | batch descriptor mid/control |
| Decode | `+0x158` | `+0x000c` | W | doorbell: `(head-base)<<16 \| (A<<4)\|(B<<8)\|C` |
| Decode | `+0x158` | `+0x0008` | R | status word (bit0 = ready) |
| Decode | `+0x158` | `+0x0018` | W | `0x1f` "submitting" marker |
| Bitfile| `+0x148` | `+0x0014` | R | interrupt status (bit0 = pending) |
| Bitfile| `+0x148` | `+0x0010` | R | secondary status (logged) |
| Bitfile| `+0x148` | `+0x0020` | R/W | status; ack = write back `(val & 7)` |
| Bitfile| `+0x148` | `+0x0004` | W | clear-on-interrupt control (set to 0) |

**Descriptor tag:** `0xAD4B` in the top 16 bits of each descriptor's control word;
final-descriptor flag = `0x13` OR'd into that field.

---

## 9. SPI / bitfile programming (annotated) — flash write state machine

The Flash region (`self->+0x150`) is a small command/status block, not raw memory-mapped
flash. The driver programs it by writing command words and polling status bits. Three
functions drive it: `spierase`, `spiprogram`, and (indirectly) `loadBitfile`.

### 9.1 Flash region register map (`self->+0x150`) — complete sweep

Confirmed by sweeping every function that loads the Flash base (`updateIORegistryProperties`,
`readFlashRegister`, `writeFlashRegister`, `spierase`, `spiprogram`). The generic accessors
(`read/writeFlashRegister`) do a raw 32-bit read/write at an arbitrary offset, so the region
is fully addressable; the fixed offsets below are what the driver's own code actually uses.

| Offset | Access | Meaning | Used by |
|---|---|---|---|
| `+0x0010` | R | **status / busy flags** — bit 4 (`0x10`) = "busy, poll"; (bit 1/2 also seen in program path) | all |
| `+0x0040` | W | command address (erase addr / program offset / read addr) | spierase, spiprogram, updateIOReg |
| `+0x0048` | R/W | **command word** — see opcode table below | all |
| `+0x004C` | W | data word (32-bit, byte-swapped before write) | spiprogram |
| `+0x0050` | R | **read-data register** — 32-bit value read back after a READ command (byte-swapped by host) | updateIOReg |
| `+0x0054` | R/W | completion / done flag — set to 1 then cleared; polled for "op finished" | spierase, spiprogram |

**Command opcodes written to `flash[+0x48]`:**

| Opcode | Meaning | Issued by |
|---|---|---|
| `0xEEEE` | **ERASE** — erase block at `flash[+0x40]` | `spierase` |
| `(len<<16)\|0xAAAA` | **PROGRAM** — write `len` bytes from data to `flash[+0x40]` | `spiprogram` |
| `0xBBBB` | **READ / ID** — read 32-bit value at `flash[+0x40]` into `flash[+0x50]` | `updateIORegistryProperties` |

The busy-poll pattern is uniform: after issuing a command the driver loops on
`flash[+0x10] & 0x10`, calling `usleep(0x64)` (100 µs) between checks, until bit 4 clears.
(`updateIORegistryProperties` polls up to 8 times; `spiprogram`/`spierase` poll indefinitely.)

**`updateIORegistryProperties` Flash sequence** (the READ path):
```asm
movl $0x13fe000, 0x40(%rax)   ; flash[+0x40] = 0x13FE000  (read address — likely a device-ID / version register)
movl $0xbbbb,    0x48(%rax)   ; *** flash[+0x48] = 0xBBBB (READ command) ***
; poll: while (flash[+0x10] & 0x10) usleep(0x64);  [max 8 iterations]
movl 0x50(%rax), %edx         ; edx = flash[+0x50]  (read-back value)
bswapl %edx                   ; host byte-swaps it
; ... stores the swapped word into a local array, then:
movl $0x0, 0x48(%rax)         ; flash[+0x48] = 0     (clear command / deassert)
```
The read-back value is published to IOKit as part of the device's registry properties
(alongside the "Afterburner" and "FGPA" string properties built at `0x201c`/`0x2089`).

**Note on the busy bit:** `spierase`/`spiprogram` test `flash[+0x10] & 0x2`, while
`updateIORegistryProperties` tests `& 0x10`. Both are "busy" indicators — the lower bits
(`0x2`) track program/erase completion, bit 4 (`0x10`) tracks read/ID completion. The
status word is a multi-bit flag register, not a single busy line.

### 9.2 `spierase(u32 addr)` — `0x3ec6`

Erase one flash block at `addr`:
1. Log entry (string `0x8d41`).
2. Issue erase command:
   ```asm
   movl $0x1,  0x54(%rax)     ; flash[+0x54] = 1   (arm completion flag)
   movl $0x0,   0x54(%rax)    ; flash[+0x54] = 0   (clear — re-arm / reset)
   movl %esi,   0x40(%rax)    ; flash[+0x40] = addr
   movl $0xeeee,0x48(%rax)    ; *** flash[+0x48] = 0xEEEE (ERASE command) ***
   ```
3. **Busy-poll**: while `flash[+0x10] & 0x2` → `usleep(0x64)` and re-check.
4. On completion: write the done-flag back (`flash[+0x54] = 0`, then `= 1`) and log exit
   (string `0x8dd4`).

The erase command token is **`0xEEEE`** — a classic SPI flash sector-erase opcode pattern.

### 9.3 `spiprogram(u32 addr, u32 len, void* data, bool swap, bool flag)` — `0x3fbe`

Program `len` bytes from `data` to flash address `addr`. This is the real state machine:
1. Log entry (string `0x8e0b`).
2. **Issue program command**:
   ```asm
   movl %edx,  0x40(%rcx)     ; flash[+0x40] = addr
   shll $0x10, %ebx           ; ebx = len << 16
   orl  $0xaaaa, %ebx         ; *** command = (len<<16) | 0xAAAA (PROGRAM opcode) ***
   movl %ebx,  0x48(%rcx)     ; flash[+0x48] = command word
   ```
3. **Data-write loop** (processes `len` bytes in 4-byte chunks):
   - Reads each 32-bit word from `data`, applies `bswapl` if the `swap` flag is set,
     then writes it to `flash[+0x4C]`.
   - After every **16 words** (when `(chunk_idx & 0xffffc) == 0xfc`) it logs progress
     (string `0x8e55`) and advances the address by 8.
   - Busy-polls on `flash[+0x10] & 0x2` between chunks (`usleep(0x64)`).
4. **Final word** (if `len % 4 != 0`): reads the trailing bytes into a local, byte-swaps
   if needed, writes to `flash[+0x4C]`, then sets `flash[+0x4C] = 0xFFFFFFFF` (pad/EOS).
5. **Completion poll**: loops on `flash[+0x10] & 0x2` (busy) and `& 0x4` (done), with
   `usleep(0x64)` between checks, logging progress (strings `0x8ec8`, `0x8f23`).
6. On done: clears the command word (`flash[+0x48] = 0`), re-arms the completion flag
   (`flash[+0x54] = 1` then `= 0`), and logs exit (string `0x8f86`).

**Program opcode:** `(len << 16) | 0xAAAA`. The length is encoded in the top 16 bits of
the command word; `0xAAAA` marks it as a program (vs `0xEEEE` for erase).

### 9.4 `loadBitfile(const char* path, u32 variant)` — `0x42f2`

High-level entry that loads a bitfile from disk and programs it:
1. Log entry (string `0x959e`).
2. Calls **`OSKextGetCurrentIdentifier()`** to get the kext's bundle ID, then invokes an
   external resource loader with a callback (`resourceLoadCallback`) — this is how the
   bitfile bytes are fetched from the kext bundle (not a direct `open()`/`read()`).
3. The loader returns a **variant code** in `%eax`:
   - `1` → store result at `self->+0x11a8`
   - `2` → store at `self->+0x11ac`
   - `3` → store at `self->+0x11b0`
   - else → log failure (string `0x9635`) and return the error.
4. The three destination fields (`+0x11a8/11ac/11b0`) are per-variant bitfile handles,
   selected by the `variant` argument.

### 9.5 `resourceLoadCallback(int id, const char* name, u32 len, void* ctx)` — `0x43fa`

The IOKit resource-load callback (called by the kext loader to fetch a bundled file):
1. Looks up the class via `gMetaClass` (`0xc6e0`) and validates it's non-null; on NULL →
   log failure (string `0x96d8`) and return 0.
2. Logs the load request (string `0x9674`).
3. Delegates to **`resourceLoadCallbackHandler(id, name, len)`** (`0x4484`) which performs
   the actual file read and returns the byte count (or error).

---

## 10. The "kick helper" — RESOLVED: it is `kernel_debug()`, not a kick at all

The values `0x2B610FB0`, `0x2B610FAC`, `0x2B610FB4` (x86_64 slice) and
`0x2B61FA4`, `0x2B61FAC`, `0x2B61FB0` (arm64e slice) seen in `submitDataGated`,
`handleInterrupt` and the DMA/memory paths are **NOT** dispatch tags for a hardware
notify primitive. They are **KDEBUG trace IDs** passed as arg 1 to XNU's standard
kernel tracing hook:

- The call goes through stub `0xea04` (arm64e) → GOT slot → import[49] =
  **`_kernel_debug`** (`bsd/sys/kdebug.h`: `void kernel_debug(int, int, uint64_t x5)`).
- Verified via dyldv5 chained-fixup import table: all 368 imports decode cleanly with
  the Apple `fixup-chains.h` encoding (lib_ordinal=bits[7:0], weak=bit8,
  name_offset=bits[30:9]); import[49] = `_kernel_debug`. Full table saved to
  `imports_arm64e.txt`; all C strings in `strings_arm64e.txt`.
- KDEBUG ID layout (per kdebug.h): `[class:8][subclass:8][code:14][qual:2]`,
  class = bits[31:24]. Class **0x2B** (x86_64 slice) is vendor-private — not in Apple's
  defined classes. The arm64e slice uses a different namespace entirely
  (class=0x02, sub=0xB6, codes 0x7EB1/0x7EB9/0x7ECC vs x86_64's 0x3EB1/0x3EB9/0x3ECD) —
  the low byte of each ID is identical across slices (B0/B0, AC/AC, B4/A4), so the two
  slices were evidently built with different KDEBUG class assignments. Functionally
  irrelevant: `kernel_debug` is a **no-op unless kernel tracing is enabled**.

**Consequence:** these call sites are pure debug instrumentation — they do NOT kick
hardware and play no role in DMA/interrupt flow. The real doorbells remain the BAR1
writes (0xFFE7F / 0x80FFE7F, §9/§20). For a Linux mediator: ignore them, or mirror as
ftrace tracepoints if desired.

### KDEBUG event map (arm64e slice — all 16 sites, labeled by nearest IOLog string)

| ID | Sites | Event (from adjacent log strings) |
|---|---|---|
| `0x2B61FAC` code=1 | @0x5bac | **bitfile interrupt** received (`"bitfile interrupt: 0x%08x"`) |
| `0x2B61FAC` code=2,4 | @0x6398, @0x63d0 | **exiting thermal distress state** (two exit paths) |
| `0x2B61FAC` code=3 | @0x6474 | **Error from decoder: 0x%08x!** |
| `0x2B61FB0` code=1 | @0x9bb8 | **Wake** event (`"Wake: 0x%08x"`) |
| `0x2B61FA4` arg1∈{3,4,5} | @0xd858, @0xe078 | **"no provider or inactive"** (DMA memory setup) |
| `0x2B61FA4` arg1∈{4,5} | @0xdaac, @0xdae0, @0xdb38 | **"failed to wire memory: %u"** |
| `0x2B61FA4` arg1=3 | @0xe298 | **"failed to get memory descriptor"** |
| w22-base+{4,8} (dynamic) | @0x65b4, @0x6624, @0x66f8, @0x67f8, @0x69c4 | **DMA frame events**: "Offending frameNum: 0x%016llx" / "Completion for frameNum %llu, errF…" — the ID is built at runtime from a per-context base (w22) + offset, i.e. one tracepoint family parameterized by context/frame |

The x86_64 slice has the same families: `0x2B610FAC` ×4 sites (arg3=code 1..4),
`0x2B610FB0` ×3 (Wake; arg4=1, arg5=0, arg6=[self+0xC0]), `0x2B610FB4` ×3.

**Driver event model this reveals:** the kext treats exactly these as notable runtime
events — bitfile interrupts, thermal-distress entry/exit, decoder errors, DMA frame
completion/error (per-frame), memory-wiring failures at submit time, and wake-from-idle.
That is the driver's internal state machine in one table; a Linux mediator should log
the same events (dmesg/ratelimited) to be debuggable in the same way.

---

## 12. Deep-dive decodes (arm64e): submitDataGated, SPI flash, handleInterrupt

All addresses below are arm64e-slice VAs (= file offsets; `__text` is 0x4000–0xe83c).
Function boundaries located via string-reference correlation + retab/ret markers.

### 12.1 `submitDataGated` (≈0x97f0–0x9bc0) — the gated submit state machine

Args: x0=self, x1=sDmaConfig*. The **sDmaConfig** layout (decoded from all field
accesses):

| off | size | meaning |
|---|---|---|
| +0x00 | 8 | (pending-list link slot; written when queued) |
| +0x10 | 8 | descriptor PA (lo/hi) → doorbell BAR1+0x4080/+0x4084 |
| +0x18 | 4 | control word → doorbell BAR1+0x4088; also copied to desc_obj->+0x18 on slot alloc |
| +0x1C | 4 | mode: 0=single, 1=batch (selects direct submit vs queue) |
| +0x20 | 4 | ring slot index (indexes the free-list array) |
| +0x30/34/38 | 4×3 | descriptor field bits → desc[+0xC] low 16: {cfg30:4, cfg34:4, cfg38:8} |
| +0x3C | 4 | valid flag → copied to self->+0x12E4 (pending-frame count) |
| +0x40 | 8 | pointer to the 32-byte descriptor object in DMA memory |

Flow:
1. **Wake path** — if `self->+0x12F8 == 0` and mode==0: call vtable[0x200](self->+0x90,
   &flag, 2) in a loop until non-zero (wake the card), log `"Wake: 0x%08x"`, return.
2. **Busy check** — if `self->+0x12C8 != 0` → return 0 no-op (a frame is already in
   flight; +0x12C8 is set by the ISR on completion, §12.3).
3. **Backpressure / hang detection** — while `self->+0xC0 >= 32` (frame counter): if
   mode!=0 submit directly; else call vtable[0x210](self->+0x90, &local, self, 2) with a
   2000-unit timeout (w1=0xF4240); return value 1 → log `"Possible hang, frameNum %llu,
   completionStamp %lld"` and bail.
4. **Descriptor field write** — if `self->+0x158` (current-frame context) is non-NULL:
   build word `{(frame_delta)<<16 | cfg38<<8 | cfg34<<4 | cfg30}` where
   frame_delta = self->+0xC8 − self->+0x1308, store at **desc[+0x0C]**.
5. **Free-list / slot management** — array base `self + 0x11F0`, stride 16 bytes,
   **8 slots** (spans +0x11F0…+0x1270). Slot entry is a pointer; self-referencing
   (`*entry == &entry`) means free. Per-slot flag byte at `self + 0x11E4 + slot`.
   - Free slot: copy config[+0x10] (64-bit) → desc_obj->+0x18, clear bit 0 of
     desc_obj->+0x00 ("pending" flag), increment a counter at [entry_val]+0x3C.
   - Occupied slot: **queue** — link this sDmaConfig into the pending chain stored at
     `self + 0x11F8 + slot*16` (chain nodes carry next-ptr at node+8), return 0x19 (25).
6. **Doorbell** (mode==0, direct): BAR1+0x4=0; BAR1+0x4080/4/8 = PA_lo/PA_hi/control;
   BAR1+0x4 = **0xFFE7F**. Mode==1 uses the second writer (~0x9B34) with +0x1004 /
   +0x5080-88 and 0x80FFFE7F.

**Descriptor object (32 bytes, DMA memory)** — fields confirmed so far:

| off | meaning |
|---|---|
| +0x00 | status word; bit 0 = pending (cleared on slot alloc) |
| +0x04 | ack/done flag: ISR writes 0 → wait 1 ms → 1 (§12.3); submitData checks it |
| +0x08 | ready flag (bit 0), checked by `submitData` before arming |
| +0x0C | {frame_delta:16, cfg38:8, cfg34:4, cfg30:4} — written per submit (§12.1.4) |
| +0x10 | frame-index word; **>>16 = ring slot index** (ISR uses it to advance the ring) |
| +0x14 | completion status (low 5 bits ≠ 0 ⇒ complete; bit 0 ⇒ success); read by ISR |
| +0x18 | 0x1F written by `submitData`; overwritten with config[+0x10] on slot alloc |

`getDescriptorList` (~0x9C5C) allocates **count × 32 bytes** via the IODMACommand
helper — confirming the 32-byte descriptor stride.

### 12.2 SPI flash protocol (spierase ≈0x80E0–0x8210, spiprogram ≈0x8260–0x8570)

The **SPI control block** is `self->+0x150` (a BAR mapping; physical offset pending
live-card verification — see §13). Register map:

| off | name | semantics |
|---|---|---|
| +0x10 | status | bit 1 = wrfifo_full, bit 2 = busy/done, bit 5 = erase-busy |
| +0x40 | address/param | erase target addr; program flash start addr (arg2) |
| +0x48 | command word | `0xEEEE` erase · `(len<<16)\|0xAAAA` program · `0` idle |
| +0x4C | data word | 32-bit SPI data in; **0xFFFFFFFF = end-of-data terminator** |
| +0x54 | kick | write 1 then 0 (both ops, at start and cleanup) |

**Erase sequence** (`spierase(self, addr)`):
```
obj->+0x40 = addr
obj->+0x48 = 0xEEEE
while (obj->+0x10 & (1<<5)): delay(1 ms)      # erase-busy
obj->+0x48 = 0; obj->+0x54 = 1; obj->+0x54 = 0
```

**Program sequence** (`spiprogram(self, len, addr, buf, bytes, endian)`):
```
obj->+0x40 = addr
obj->+0x48 = (len << 16) | 0xAAAA            # len logged as %u ("begin programming flash-%u")
for each 32-bit word of buf:                  # tail <4 bytes padded with 0xFF
    while (obj->+0x10 & (1<<1)): delay(100 µs)   # wrfifo_full — log on every wait
    obj->+0x4C = word                          # optional byte-reverse per endian arg
obj->+0x4C = 0xFFFFFFFF                       # terminator
while (!(obj->+0x10 & (1<<2))): delay(100 µs)  # busy/done — log "wrfifo is not empty"
delay(10 ms)                                  # settle
obj->+0x48 = 0; obj->+0x54 = 1; obj->+0x54 = 0
```

This matches and extends §9.1 (opcodes `0xEEEE` / `(len<<16)|0xAAAA`; the read/ID
opcode `0xBBBB` at addr 0x13FE000 comes from the x86_64 slice analysis). For a Linux
mediator this is directly reproducible: mmap the SPI block, issue command words, poll
status — no IOKit involved.

### 12.3 `handleInterrupt` (0x5AEC–0x6974) — full interrupt dispatch

The **interrupt status block** is `self->+0x148` (BAR mapping):

| off | name | semantics / ack protocol |
|---|---|---|
| +0x04 | ack/clear | write 0 to acknowledge (used for reg-3 bit 1/2 events) |
| +0x10 | status A | bit 0 = event type 2; logged, no explicit ack seen |
| +0x14 | status B | bit 0 = event type 1 ("bitfile interrupt"); kdebug code 1 |
| +0x20 | status C | bits 0–2 = DMA/frame events; **ack = write back `val & 7`** |
| +0x24 | status D | error bits, mask 0x7D (bits 0,2–6); ack/clear = write back masked val |

Dispatch order: read all four regs → log each non-zero one (IOLog) → kdebug(0x2B61FAC,
self, code∈{1,2}, …) → acknowledge. Then per-event handling:

**Status C bit 1 — DMA frame completion:** clear `self->+0x158`, then walk a
**hardware linked list in BAR0**: start at index 0; each entry is a u32 at
`BAR0 + idx`; low byte == 1 ⇒ channel ready (set flag self->+0x11D0, record
`self->+0x158 = &BAR0[idx]`); next index = `(entry >> 14) & 0x3FFFC + idx`; stop at
low byte == 0. For the recorded entry: read `*p`, write `*(p+4)=0`, wait 1 ms, write
`*(p+4)=1` (ack handshake in BAR space).

**Completion tail** (flag self->+0x11B4 set ⇒ channel-walk path):
```
w25 = BAR1[+0x2044]                 # channel completion status
BAR1[+0x2018] = w25; BAR1[+0x2014] = w25   # ack (two writes)
n = w25 >> 1:
    n == 0 → single frame: st = BAR1[+0x44]; read BAR1[+0x48] (clears)
    n == 1 → batch:         st = BAR1[+0x1044]; read BAR1[+0x1048] (clears)
    else   → error path (st = 0)
```

**Descriptor completion** (no channel flag): `desc = self->+0x158`; if
`desc[+0x14] & 0x1F`: kdebug code 3; if bit 0 set: call helper @0xBE64(self), then
**set `self->+0x12C8 = 1`** (the "busy" flag that gates submitDataGated §12.1.2!), and
advance the ring:
```
frame_idx = desc[+0x10] >> 16
next_pa   = self->+0x1308 + frame_idx + (self->+0xD8 << 16)   # ring base + slot + stride
self->+0x12D0 = next_pa; self->+0x12D8 = 0
```

**Batch completion loop:** while `self->+0x12E4 > 0` (pending count): recompute
next_pa into self->+0x1270, kdebug(0xFB4, …), decrement +0x12E4 and +0xC0, then call
**vtable[0x208](self->+0x90, self, 0)** — the per-frame completion callback.

**Error accumulation:** `self->+0x160 |= (statusC & 0x7D)` for new errors;
`self->+0x160 &= ~(statusD & 0x7D)` when bits clear (recovery). First-error and
all-clear events are logged.

**Re-submit from ISR:** after ack, the handler dequeues a pending sDmaConfig from the
free-list chain (§12.1.5) and rings the doorbell inline: BAR1+0x4=0; +0x4080/4/8 =
PA_lo/hi/control (from config[+0x10]/[+0x18]); **BAR1+0x4 = 0xFFE7F** — identical to
the direct path. Mode==1 takes the batch doorbell variant at ~0x6978.

### 12.4 What this means for the Linux mediator

- The whole runtime protocol is plain MMIO + DMA memory: no IOKit-specific magic.
  A kernel driver can implement: init (§7), submit (doorbell sequence), completion
  polling/interrupts (status block ack protocol), and SPI bitfile updates (§12.2).
- Completion notification has two usable forms: MSI/INTx via the status block, or
  pure polling of BAR1+0x44 / +0x1044 / +0x2044 — the kext itself polls in several
  paths (queryCompletionGated), so a poll-first mediator is faithful.
- The 32-byte descriptor stride and ring arithmetic (`base + idx + (stride<<16)`)
  must be replicated exactly for DMA buffer layout.
- Open: physical BAR+offset of the `self->+0x148` (interrupt) and `self->+0x150`
  (SPI) blocks — resolve by disassembling arm64e `start()` or probing the live card
  (§13).

---

## 12b. Frame submission pipeline — FULLY DECODED (arm64e)

Complete decode of the runtime frame-submission path from `UserClient::submitFrame` down to
descriptor-chain construction and chain-tail update. This is THE path a Linux mediator must
replicate for live decode workloads. All addresses are file offsets in
`/tmp/ab_arm64e_full.macho` (VA==fileoff).

### 12b.1 Object model: one big class, many instances

Every object on this path authenticates its vptr with PAC constant **0xcda1** — a single
large class (~0x1300+ bytes) instantiated per decoder channel. `UserClient->+0xd8` is the
primary instance; the collection at `instance->+0x90` holds sibling instances (one per
channel — consistent with the 8 channel blocks at BAR0+0x8000..+0xf000, stride 0x1000).

Key fields of a decoder instance:

| Offset | Meaning |
|---|---|
| +0x90 | collection (OSArray-like) of sibling instances / completion records |
| +0xa0 | segment manager #1 — vtbl[+0x148] get-segment, vtbl[+0x150] release |
| +0xa8 | segment manager #2 (same vtable slots; used by stage2) |
| +0xb0 | allocator context for `0xe8f0` (segment allocation fallback) |
| +0xc0 | queue depth counter — bounded at **32** (0x20); full → backoff via vtbl[+0x210] |
| +0xc8 | frame handle counter (handle = `+0xc8 - +0x1308`; base at +0x1308) |
| +0x11d0 | "decoder configured" flag byte — 0 → frame is DROPPED via receive path |
| +0x11d8 | ptr to per-segment array: N × 16-byte {data_ptr(8), context_word(4)} entries |
| +0x11e0 | segment count field (>>4 = descriptor/segment count) |
| +0x12b8 | queue allocator object — `0xe86c` reserve / `0xe878` release (lock, not free) |
| +0x12c8 | busy/submitted flag byte — set → submit rejected with 0xe000_02d8 |
| +0x13b8 | per-queue depth counter (insert callback; max 32 with backoff) |
| +0x140 | ptr to register block (BAR0 main region) — thermal section below |
| +0x158 | "decoder ready" state object: null or bit0 of [+8] clear → reject 0xe000_02db; writable command word at +0xc |
| +0x160 | thermal distress counter — non-zero → drop frame + diagnostic dump |
| +0x169 | "flash update in progress" byte (logged when dropping unconfigured frames) |
| +0x1278 | SEND chain-tail field (descriptor DMA address; see 12b.3) |
| +0x1280 | RECEIVE chain-tail field |

**Thermal register section** (relative to `instance->+0x140` register block — BAR0 main region):

| Reg offset | Log string | Decode |
|---|---|---|
| +0x10 | "thermal status live: 0x%08x" / "dropping frame due to thermal distress condition 0x%08x" | distress/status word (checked per-submit via instance->+0x160) |
| +0x80 | "thermal backcard ADC: %u C" | temperature ×128 (°C = val>>7) |
| +0x84 | "thermal FPGA die ADC: %u C" | ditto |
| +0x88 | "thermal faceplate ADC: %u C" | ditto |
| +0x8c | "thermal bottom ADC: %u C" | ditto |
| +0x94 | "thermal sysmon ADC: 0x%08x" | raw |

### 12b.2 Pipeline overview

```
UserClient::submitFrame(idx, in, out)          [UserClient method]
  └─ slot = *(self+0xf0 + idx*24)              [PAC'd decoder instance ptr]
     submitWorkhorse @0xa9ac (decoder, qb, entry, slot_obj, out, ...)
       ├─ check decoder->+0x12c8 busy → err 0xe000_02d8
       ├─ reserve queue lock: 0xe86c(decoder->+0x12b8)
       ├─ thermal check: decoder->+0x160 != 0 → dump 6 thermal regs, abort
       └─ submitSendReceive @0xa67c (decoder, qb, entry/slot, flag, out, ...)
            │   flag = 0 if decoder configured (+0x11d0 set) else 1
            ├─ vtbl[+0x8f8](decoder->+0x88, 1)          [lock/prepare]
            ├─ ready check: decoder->+0x158 null or bit0([+8]) clear
            │     → "Decoder not ready!! (0x%08x)" err 0xe000_02db
            ├─ stage1 @0xa28c "obtain segments"
            │    mgr = decoder->+0xa0; seg = vtbl[+0x148](mgr,0) or alloc 0xe8f0(…,0xfffffc0)
            │    vtbl[+0x148](seg, entry, 0)             [associate]
            │    vtbl[+0x160](seg, a7, a8, 1, 1)         [wire memory]
            │    fail → vtbl[+0x150](mgr, seg) release; "could not obtain segments: %u"
            ├─ stage2 @0x9c5c "obtain descriptor list"   ← builds the chain (below)
            │    fail → "could not obtain descriptor list: %u"
            └─ success: alloc 72-byte completion record via 0xe890(0x48):
                 rec+0x10 = first-desc DMA addr (from stage2 out_ptr)
                 rec+0x18 = local value, rec+0x1c = flag
                 rec+0x20 = is_receive (flag bool)
                 rec+0x28 = decoder->+0xc8 (handle counter snapshot)
                 rec+0x30..0x37 = slot-related values
                 rec+0x38 = arg8, 1
                 rec+0x40 = old chain-tail value read from +0x1278/+0x1280 BEFORE stage2
                 → insert into collection via vtbl[+0x1e8](coll, cb@0x985c, rec, 0,0,0)
       └─ release queue lock: 0xe878(decoder->+0x12b8)   [both success and failure]
```

Log strings (error paths): "failed to submit send data: %u" @0x28cd (flag=0),
"failed to submit receive data: %u" @0x2911 (flag=1), function name "submitData" @0x25e3.

**The configured/not-configured split:** when `decoder->+0x11d0` is clear the kext logs
"dropping frame as decoder is not configured. flash update in progress: 0x%08x" (value =
byte at +0x169) and submits via the **receive** path instead — i.e., an unconfigured
decoder's frames are dropped by submitting a receive/discard operation, keeping the
pipeline in sync.

### 12b.3 stage2 @0x9c5c — descriptor chain construction (the core)

Args: (decoder, queue_base, seg_array_ptr=+0x11d8, count=(+0x11e0)>>4, flag, &out_first_pa,
&remaining_out, 0).

1. **Allocate descriptor buffer**: `0xe95c(3, count*32, 0x1000)` — type-3 allocation of
   count×**32 bytes** (descriptor size) + 4KB header region. Null → err 0xe000_02be
   ("could not obtain descriptor list").
2. **Get segment**: `vtbl[+0x148](decoder->+0xa8, 0)`; null fallback allocates via
   `0xe8f0(decoder->+0xb0, 0, 0x1000, 0, 0, 1, 0)`.
3. **prepareMemoryDescriptor**: `vtbl[+0x218](desc_buf, 0)` [const 0xf5b3] — fail →
   "could not prepare memory descriptor: %u" @0x2412.
4. **prepareDMACommand**: `vtbl[+0x148](seg, desc_buf, 1)` [const 0xb91] — fail →
   "could not prepare DMA command: %u" @0x245f. (These vtable implementations are where
   the BAR1 MMIO programming from §7/§12.1 happens.)
5. **Query mapping**: `vtbl[+0x308](desc_buf)` → CPU virtual base; `vtbl[+0x1f0]` → size
   (0 → fallback pending-list path, see below); `vtbl[+0x178](desc_buf, &dma_base, &n, &flg)`
   [const 0x3968] → DMA/bus base address. Fail → "could not obtain descriptor list segments: %u" @0x24a6.
6. **Build the chain** (count>0): for i in 0..count-1, at CPU addr `vbase + i*0x20`:

   | Desc offset | Value |
   |---|---|
   | +0x00 | `0xAC4B` \| ((remaining-1) << 8) — magic **0xAC4B** + remaining count in bits [15:8]; final descriptor has low byte **0x13** (terminator), others 0x00 |
   | +0x04 | context_word = *(seg_array + i*16 + 8) |
   | +0x08 (SEND, flag=0) or +0x10 (RECEIVE, flag=1) | data_ptr = *(seg_array + i*16) — per-segment DMA address |
   | +0x18 | next descriptor's **DMA** address (`dma_base + 0x20 + i*0x20`); final desc: 0 (NULL) |

   - `out_first_pa` = dma_base of first descriptor → stored in completion record +0x10.
   - After the chain is built, **the terminator descriptor's address is written to
     `decoder + flag*8 + 0x1278`** (SEND) / `+0x1280` (RECEIVE). This field was read into
     completion record +0x40 *before* stage2 ran — it is a software chain-tail pointer,
     not the MMIO doorbell itself. The real hardware doorbells remain BAR1+0x0004=0xFFE7F /
     BAR1+0x1004=0x80FFE7F per §12.1 (written inside the prepare* vtable methods).

7. **Fallback path** (vtbl[+0x1f0] size==0): allocates a 24-byte node
   {&list_head_field, next, desc_buf} via `0xe890(0x18)` and appends it to a per-queue
   linked list in the queue_base object (send: head +0x20/tail +0x28; receive: +0x30/+0x38),
   under a reserve/release of `*(queue_base+flag*8+0x50)` via 0xe86c/0xe878. Pending frames
   waiting on descriptor memory.

### 12b.4 Completion records & query path (corrections)

- **Completion record** = 72-byte object from `0xe890(0x48)`; layout in 12b.2. Inserted into
  the collection at `decoder->+0x90` via vtbl[+0x1e8] with callback @0x985c (bounded-queue
  insert: depth counter at +0xc0, max **32**; when full → backoff call vtbl[+0x210](coll,
  &counter, timestamp, 2) and retry; send-path also writes a packed descriptor word to
  `decoder->+0x158 + 0xc` = ((handle-base)<<16 | rec_field<<8 | two more fields)).
- **queryCompletion** uses the same vtbl[+0x1e8] with comparator callback @0xb634 — a
  binary search over frame-table elements {flag, stored handle, counter}; empty slots are
  marked with self-pointer sentinels.
- **0xb88c is NOT the completion workhorse** (earlier misidentification): it is a leak-check
  diagnostic that walks dmaQ/descDmaQ queues using self-pointer sentinels for empty slots.

### 12b.5 Error-code convention

Codes are `0xe000_02xx` (kext-local range). Observed: 0x2be = allocation failure
(descriptor list / segments), 0x2bc = insert-queue busy, 0x2db = decoder not ready,
0x2d8 = frame busy/thermal drop.

### 12b.6 What the Linux mediator must replicate

1. Per-channel state: configured flag, thermal distress check (read regblock+0x10 / +0x160),
   queue depth ≤32 with backoff.
2. Scatter-gather segment array (N × {data_ptr, context_word}).
3. 32-byte descriptor chain in DMA-coherent memory: magic 0xAC4B, remaining count, per-segment
   data ptr at +8 (send) / +10 (receive), next-DMA-ptr at +18, terminator low byte 0x13.
4. Chain-tail pointer update (+0x1278/+0x1280 equivalent) and completion-record bookkeeping
   for the query path.
5. The actual BAR1 MMIO doorbell sequence per §7/§12.1 (inside prepareMemoryDescriptor /
   prepareDMACommand — next disassembly target: those vtable implementations).

### 12b.7 Next targets

- Vtable implementations behind constants 0xf5b3 (+0x218), 0xb91 (+0x148), 0xfc62 (+0x160),
  0x3968 (+0x178), 0xf4f4 (+0x308), 0xb392 (+0x1f0) — the segment/desc-buffer class; these
  contain the real BAR1 writes.
- `0xe95c` (type-3 allocator: size, 0x1000 extra) and `0xe8f0`/`0xe86c`/`0xe878` queue ops.
- The +0x158 state object's vtable (+0xc command word write in the insert callback).

---

## 11. Updated open questions / next steps

1. ~~The notify/kick primitive~~ — **RESOLVED (§10):** it is `kernel_debug()`, XNU's
   no-op tracing hook, not a hardware signal path. No further work needed.
2. ~~Flash region full map~~ — **DONE.** Complete sweep in §9.1: six fixed offsets
   (`+0x10/+0x40/+0x48/+0x4C/+0x50/+0x54`), three opcodes (`0xEEEE` erase, `(len<<16)|0xAAAA`
   program, `0xBBBB` read/ID), and the multi-bit busy-flag semantics. Full arm64e
   protocol (spierase/spiprogram) in §12.2.
3. ~~Two DMA paths~~ — **RESOLVED (§12.1):** both are real and selected by
   `sDmaConfig[+0x1C]` mode: 0 = single-shot → `+0x40xx` bank, doorbell
   `BAR1+0x0004=0xFFE7F`; 1 = batch → `+0x50xx` bank, doorbell
   `BAR1+0x1004=0x80FFE7F`. Confirmed in both slices.
4. ~~BAR1 status-poll registers~~ — **RESOLVED (§12.3):** single = `BAR1+0x44`
   (status) / `+0x48` (read-to-clear); batch = `+0x1044`/`+0x1048`; channel =
   `+0x2044`. `queryCompletionGated` polls these; the ISR acks via the same block.
5. ~~Phase 3g submit failure~~ — **RESOLVED (Finding 8 CORRECTION):** wrong doorbell
   register/value (+0x2004/0x16 is init, not DMA) and wrong desc-pointer register
   (+0x3080 is never written by the kext). Correct sequence in §7.4/§12.1 — a Linux
   driver can submit real DMA with `dma_alloc_coherent` + MMIO only.
6. ~~Thermal snapshot fields~~ — **RESOLVED (§12b.1):** the six per-submit words are the
   Thermal-region registers `+0x10` (distress status), `+0x80/+0x84/+0x88/+0x8c` (backcard /
   FPGA-die / faceplate / bottom ADC temps, ×128 → °C = val>>7) and `+0x94` (sysmon raw).
   Log strings "thermal backcard/FPGA die/faceplate/bottom ADC: %u C" + "thermal sysmon ADC".
7. ~~Ring wrap-around~~ — **RESOLVED (§21):** the depth-32 bound is a *software* cap in the
   kext's bounded completion queue (counter `self->+0xc0`, post-increment load, compared to 0x20,
   decremented per completion in the handler @~0x5aec) — it is NOT a hardware ring size. The
   descriptor chain itself is an arbitrary-length linked list in system RAM (`count*32` bytes
   allocated per submit, §12b.3), so the card's DMA engine walks a pointer chain with no fixed
   depth visible to software. A Linux driver can use any queue depth; 32 is just Apple's choice.
8. **Physical BAR+offset of `self->+0x148` (interrupt) and `self->+0x150` (SPI) blocks** —
   resolve by disassembling arm64e `start()` or probing the live card (§13).
9. **Meaning of `local_val` (rec+0x18, staged to BAR1+0x4088/+0x5088)** — 32-bit value from
   submitSendReceive's stack (sp+0x24); likely a frame number or completion stamp (cf. the
   "Possible hang, frameNum %llu, completionStamp %lld" log). Trace its origin in @0xa67c if the
   live retest needs it to match kext behavior exactly.

---

*Checkpoint: BAR-accessor sweep complete, seven real register families mapped,
init/probe/start annotated, full DMA submit path documented (doorbells 0xFFE7F /
0x80FFE7F at +0x0004/+0x1004, descriptor words at +0x4080..+0x5088), and the SPI/bitfile
programming state machine captured with a complete Flash-region sweep (§9.1): erase 0xEEEE,
program (len<<16)|0xAAAA, read/ID 0xBBBB at addr 0x13FE000, read-back via flash[+0x50],
multi-bit busy flags on flash[+0x10]. The "kick helper" values are KDEBUG trace IDs passed
to `kernel_debug()` — XNU tracing, not hardware (§10). Phase 3g's three errors corrected;
arm64e deep-dive of submitDataGated/SPI/handleInterrupt in §12.*

*Checkpoint: full frame-submission pipeline decoded end-to-end (§12b): UserClient::submitFrame →
submitWorkhorse @0xa9ac (busy/thermal gates) → submitSendReceive @0xa67c (lock, ready check,
stage1 segments @0xa28c + stage2 descriptor chain @0x9c5c) → 32-byte linked descriptors with
magic 0xAC4B / terminator byte 0x13 / per-segment data ptrs from the N×16-byte array at
instance->+0x11d8 → chain-tail stored to +0x1278 (send) / +0x1280 (receive); 72-byte completion
records inserted into a depth-32 bounded queue with backoff; unconfigured decoders drop frames
via the receive path. Thermal register offsets mapped (+0x10/+0x80..+0x94 of the +0x140 block).
Open: vtable implementations behind constants 0xf5b3/0xb91/0xfc62 (the real BAR1 writers), and
allocators 0xe95c/0xe8f0/0xe86c/0xe878.*

---

## 21. BAR1 runtime protocol — submit + completion, FULLY DECODED (arm64e)

**CORRECTION to the RE-side "BAR1 is empty" note (Phase 0 cold read; superseded live in §19):**
a full `str`/`ldr` census of every access through a base register loaded from `self->+0x130`
(the BAR1 ioremap) across all of `__text` shows **BAR1 is the live command / doorbell / status
surface** — descriptor staging, doorbells, and completion status all live here. It only looks
empty at cold boot because nothing has written it yet. This section gives the exact runtime
protocol a Linux mediator must replicate, from the arm64e slice (complementing §19's live-hardware
findings).

### 21.1 The two submission functions (confirmed by log strings)

| Function | VA | Log-string evidence | Role |
|---|---|---|---|
| `submitDataGated` | **0x985c** | "submitDataGated" @0x2357; "Possible hang, frameNum %llu, completionStamp %lld" @0x2367 | Builds the 3-word descriptor, stages it in BAR1, rings the doorbell. This is the **queue-insert callback** (vtbl[+0x1e8] cb@0x985c from §12b.4). |
| completion/reclaim handler | **~0x5aec** (large fn) | "Error from decoder: 0x%08x!" @0x2f4c; "HARD error from decoder" @0x2f8d; "Offending frameNum: 0x%016llx" @0x2fd3 | Reads BAR1 completion status, acks it, processes completed frames, checks decoder errors. This is the **interrupt / poll-completion path**. |

### 21.2 Submit protocol — exact BAR1 write sequence (from `submitDataGated` @0x985c)

The descriptor is a **3-word** structure staged into one of two BAR1 banks, then a doorbell is
rung. Mode is selected by the completion-record flag field (`rec+0x1c`: 0 = **send**,
1 = **receive**) — verified at record construction (below).

**What the three words are** (verified from `submitSendReceive` @0xa67c, record build at
0xa8c8–0xa8fc):
```
rec+0x10  = str x8,[rec,#0x10]   ; FULL 64-bit store of stage2's out_first_pa (first-desc DMA addr)
rec+0x18  = local value (32-bit, from stack sp+0x24)
rec+0x1c  = flag (0=send / 1=receive)          ← selects the BAR1 bank + doorbell
```
So in `submitDataGated` (`ldp w9,w8,[ctx,#0x10]`): **word A = first_desc_PA_lo, word B =
first_desc_PA_hi**, word C = local value. The card is told *where in system RAM* the 32-byte
descriptor chain (§12b.3) starts — consistent with §16's live finding that the ring lives in
system RAM, not the BAR.

**Send mode (flag==0) — bank `+0x40xx`, doorbell `+0x4`:**
```
BAR1[+0x0004]   = 0          ; clear the doorbell first
BAR1[+0x4080]   = PA_lo      ; first descriptor DMA address, low word
BAR1[+0x4084]   = PA_hi      ; first descriptor DMA address, high word
BAR1[+0x4088]   = local_val  ; completion-record local value (rec+0x18)
BAR1[+0x0004]   = 0xFFE7F    ; RING the send doorbell
```

**Receive mode (flag==1) — bank `+0x50xx`, doorbell `+0x1004`:**
```
BAR1[+0x1004]   = 0          ; clear the receive doorbell first
BAR1[+0x5080]   = PA_lo      ; first descriptor DMA address, low word
BAR1[+0x5084]   = PA_hi      ; first descriptor DMA address, high word
BAR1[+0x5088]   = local_val  ; completion-record local value (rec+0x18)
BAR1[+0x1004]   = 0x80FFE7F  ; RING the receive doorbell (bit31 set vs send)
```

Key facts:
- **Clear-before-ring** is mandatory in both modes (write 0 to the doorbell, stage words, then
  write the magic). The magic values are `0xFFE7F` (send) and `0x80FFE7F` (receive — same low
  24 bits, bit31 set as a receive flag).
- Descriptor staging banks are **0x1000 apart**: send at `+0x4080`, receive at `+0x5080`.
- After a successful insert, the handle counter `self->+0xc8` is incremented **only for
  receive** submissions (cmp w20,#1 @0xa988).

### 21.3 Completion / reclaim protocol (from handler @~0x5aec)

```
w25 = BAR1[+0x2044]          ; read channel completion status word
BAR1[+0x2018] = w25          ; ACK by writing it back to +0x2018 (commit/ack register)
count = w25 >> 1             ; number of completed descriptors in bits [31:1]; bit0 = valid

if count == 0:               ; nothing completed — read the single-shot result pair
    w26 = BAR1[+0x44]        ;   single-shot status/result word A
    _   = BAR1[+0x48]        ;   (word B, discarded here)
elif count == 1:             ; exactly one completion — read its data word
    idx = [*(self->+0x158)+0x10] >> 16     ; frame index from the decoder-ready object
    w26 = BAR1[+0x1044]      ;   batch/completion data word
    ptr = self->+0x1308 + idx + ([self+0xd8] << 16)   ; DMA addr of completed descriptor
    self->+0x1270 = ptr      ; save "current completion pointer"
else:                        ; multiple completions — batch path (w26=0, no per-item read here)
    pass

; then decrement the in-flight bookkeeping:
self->+0xc0   -= 1           ; ring index (wraps at 32, see §12b.1)
self->+0x12e4 -= 1           ; pending-completion count
```

**Decoder error check** (same handler): reads `[*(self->+0x158)+0x14]` (decoder status word);
if `status & 0x1f != 0` → logs "Error from decoder: 0x%08x!"; if bit0 of that is set it's a
**HARD error** → calls recovery (`bl #0xbe64`), sets a flag byte at sub-object `+0x114`, and logs
"Offending frameNum".

### 21.4 Complete BAR1 register-offset census (all of `__text`)

Every distinct BAR1 offset touched by the kext, with access type and value. **Re-run this
session with proper register-lifetime tracking** (a base reg loaded from `self->+0x130` is
invalidated on any reassignment) — the earlier approximate census had false positives at
`+0xc/+0x14/...` (those were SPI-block / self-object accesses through reused registers).

| Offset | Access | Value | Meaning | Sites |
|---|---|---|---|---|
| `+0x0004` | str | 0 then `0xFFE7F` | **send doorbell** — clear, stage, ring | 0x68c0/0x68e8, 0x97c8/0x97f0, 0x9b38 |
| `+0x1004` | str | 0 then `0x80FFE7F` | **receive doorbell** — clear, stage, ring | 0x697c/0x69a4, (0x9b94) |
| `+0x4080` / `+0x4084` / `+0x4088` | str | PA_lo / PA_hi / local_val | **send staging bank** (fixed offsets, not per-channel) | 0x68c8/0x68d0/0x68dc, 0x97d0/0x97d8/0x97e4 |
| `+0x5080` / `+0x5084` / `+0x5088` | str | PA_lo / PA_hi / local_val | **receive staging bank** (mirror, +0x1000) | 0x6984/0x698c/0x6998 |
| `+0x2044` | ldr | — | **completion status** — bit0 valid, [31:1] count | 0x63a8 |
| `+0x2018` | str | = value read from +0x2044 | completion **ack/commit** (write-back) | 0x63ac |
| `+0x2014` | str | w25 (completion-path value) | config/status in the +0x20xx block | 0x666c, 0x6954 |
| `+0x2004` | str | `0x16` | **final engine enable** (init tail) | 0x5acc |
| `+0x2010` | str | `3` | engine control (init) | 0x5a50 |
| `+0x90` / `+0x1090` | str | `0xFFFFFE` | channel A/B mask (init; all-but-bit0 set) | 0x5a44/0x5a48 |
| `+0x44` / `+0x48` | ldr / ldr-discard | — | send completion data word A (+ read-to-clear B) | 0x63e4/0x63e8, 0xaf6c/0xaf80 |
| `+0x1044` / `+0x1048` | ldr / ldr-discard | — | receive completion data word (+ read-to-clear) | 0x6578/0x657c, 0xaef4/0xaf10 |

That is the **complete** set: 12 distinct offsets, 16 immediate-offset accesses plus the
register-indexed leaf accessors at 0x8034–0x8094 (`str wN,[base,wM,uxtw]` — generic vtable
MMIO methods that can target any block base). No other BAR1 offset is touched anywhere in
`__text`. In particular: **no write to `+0x3080` exists** — Phase 3g's staging-offset guess
is refuted (see §21.6).

> The doorbell / staging / completion rows are exact (verified by clean disassembly of the two
> functions in §21.2/§21.3, plus a second independent send path at 0x68bc–0x68e8 that matches
> byte-for-byte).

### 21.5 What this means for the Linux mediator

The runtime loop is now fully specified with MMIO + coherent DMA only (no IOKit):
1. **Submit:** build a 3-word descriptor, `writel(0, BAR1+doorbell)`, write the 3 words to the
   staging bank (`BAR1+0x4080` send / `BAR1+0x5080` receive), then `writel(magic, BAR1+doorbell)`
   with magic `0xFFE7F` (send) or `0x80FFE7F` (receive). Clear-before-ring is required.
2. **Complete:** read `BAR1+0x2044`, write it back to `BAR1+0x2018` to ack, decode count from
   bits [31:1]; for a single completion read the data word at `BAR1+0x1044` (receive path) or
   `BAR1+0x44` (send path). Decrement in-flight counters.
3. **Error:** poll `[decoder-ready]+0x14`; nonzero low-5-bits = decoder error, bit0 = HARD error
   requiring recovery + frameNum logging.

This closes the last open item from §12b.7 ("the real BAR1 writers") — they are not hidden in the
prepare* vtable methods; the doorbell/staging writes happen **directly inside `submitDataGated`
@0x985c**, and the completion reads happen directly inside the handler @~0x5aec.*

### 21.6 Session addendum — init/reset decode, "Wake" resolved, +0x3080 refuted (Phase 3i)

**(a) The probe-time init/reset function is fully decoded.** It lives at the tail of
`@0x55d8` (the SPI-flash ID/serial reader — one combined probe routine; no other `pacibsp`
between 0x476c and 0x5aec). After reading the device ID from flash it programs:

```
BAR1[+0x90]   = 0xFFFFFE     ; channel A mask (all-but-bit0)
BAR1[+0x1090] = 0xFFFFFE     ; channel B mask
BAR1[+0x2010] = 3            ; engine control
; BAR0 large-reg block (self->+0x140, i.e. BAR0+0x1000) — default decoder config:
BAR0[+0x1030]=BAR0[+0x1034] = 0x7D
BAR0[+0x1040..+0x106C] = {2d002a80, 2d002a80, 2a802800, 2d002a80, bcd3ba4d,
                          2f802d00, 2f802d00, 2d002a80, 2f802d00, bf58bcd3}
; BAR0 interrupt block (self->+0x148, i.e. BAR0+0x2000):
BAR0[+0x202C]=0 ; BAR0[+0x2030]=6 ; BAR0[+0x2034]=0
BAR1[+0x2004] = 0x16             ; final engine enable — LAST write of the sequence
```

**Cross-check against the live card (bar_map.txt, golden v2.62 boot):** the BAR0 decoder region
contains exactly this word vocabulary (`2d002a80`/`2f802d00`/`bcd3ba4d`/`bf58bcd3`) — the live
layout is the kext's 10-word pattern with **two extra `0xFFFF0000` "disabled entry" words**
inserted at +0x1044 and +0x105C, i.e. golden firmware programs a slightly different (2-entry)
decoder table using the same format. The magic pair `bcd3ba4d`/`bf58bcd3` matches the decoder
section-header magics from Phase 0 (`0x4DBACD3B` family). This gives us a **verified, replayable
init sequence** for Linux — but note it clobbers BAR0 golden state, so `afterburner_submit2.c`
replays only the four BAR1 words by default.

**(b) The "Wake" pre-submit requirement is fully resolved.** Call site: 0x98d8–0x98ec in
`submitDataGated` — virtual method slot (movk `0x6db9`) on the sub-object at **self->+0x90**,
args `(self+0x12f8, w2=2)`, loops (`cbz w0,#0x98a0`) until it returns non-zero. The log string
mystery is solved: the code references template **VA 0x2326** =
`"AppleAfterburner Error: %s[%p]::%s: Wake: 0x%08x"` — "Wake: 0x%08x" (VA 0x234a) is an
*embedded substring*, which is why no direct reference to 0x234a exists anywhere. The wake call
is **conditional**: it only runs when state byte `self->+0x12f8 != 0` AND the record flag
(`rec+0x1c`) == 0; otherwise submit proceeds directly (gate at 0x98a0–0x98ac, alternate path
0x9954). A fresh Linux driver has no such state object — if a first submit never completes,
wake is the next thing to chase (its implementation lives in another module's class).

**(c) Phase 3g's `+0x3080` staging offset is REFUTED.** The register-lifetime-accurate census
(§21.4) shows **no write to BAR1+0x3080 anywhere in `__text`**. The real staging banks are the
fixed offsets +0x4080/4/8 (send) and +0x5080/4/8 (receive), verified at two independent code
sites. `afterburner_init_seq.c`'s Step 2 (PA staged at +0x3080) was based on an unverified
hypothesis; the macos-capture README's "desc-ring PA at BAR1+0x3080" was a capture *plan*, never
executed (no bar_registers.txt exists).

**(d) BAR1 is not empty at cold boot.** Phase 0 dump: `BAR1+0x0 = 0x1FC08006`,
`BAR1+0x4C = 0x00010140` — engine power/clock state set by golden firmware. (The "BAR1 empty"
note in memory was wrong; only the *command surface* is zero pre-init.)

**(e) Phase 3i module built.** `linux-driver/afterburner_submit2.c` implements the verified
protocol: mode=0 read-only snapshot of all 13 key BAR1 registers, mode=1 replays the four
BAR1 init words then submits a single-descriptor send (desc[0]=0xAD4B0013, data PA at +8,
next=0; stage desc_pa to +0x4080/4/8 with local_val=0; clear→ring 0xFFE7F), polls +0x2044 for
~50ms, reads +0x44, read-to-clears +0x48, acks via write-back to +0x2018. Builds clean against
the running kernel (7.2.11-arch1). **Awaiting live run** — needs sudo for modprobe; the exact
commands are in plan.md Phase 3i.

### 21.7 Session addendum — decoder bring-up fully decoded, submit protocol VERIFIED LIVE (Phase 3i–3k)

**(a) Live verification of the §21 protocol (mode=1 and mode=3 runs).** The engine consumed
our single-descriptor submit: `+0x2010` read back **3** after init but **2** post-submit,
completion arrived with `status(+0x2044)=0x1 VALID`, count=0, `data(+0x44)=6`. The handler's
only test of `data` is bit0 (`tbnz w,#0` → skip per-frame processing); 6 = 0b110 has bit0
clear → **normal completion path**, not an error. Dual ack (+0x2018 then +0x2014) cleared the
status register to 0, matching the kext exactly.

**(b) The one real descriptor bug — fixed.** Re-tracing chain-builder @0x9f9c–0xa164 showed
`desc[+0x04]` must hold the **segment length in bytes** (raw `seg[+8]`, = 64 for our buffer),
not PA-lo. The earlier "+0x10 control word" hypothesis was disproven: ctrl 0xAD4B0013 at
`desc[+0x00]` is correct.

**(c) mode=3 settled the PA-slot question decisively.** Variant A (PA@+0x08, kext-exact send
slot) and variant B (PA@+0x10, recv slot) completed **identically** — for single-shot submits
the engine ignores which descriptor slot carries the data PA. The length field was the only
real fix.

**(d) The card is a ProRes DECODER.** Kext strings: "Decoder Control Register (enabling)"
@0x11df / "(disabling)" @0x1b48, "found decoder configuration" @0xbc2, "Could not find
decoder section header" @0x8b2. Active ProRes paths: "PR programming error" @0x2d46 and
"PR programming done" @0x2d81 (tied to bit states that trigger register writes); "PR
deprecated" @0x1174 is **dead code** (no ADRP+ADD references anywhere). Completion tracking
is by frame number.

**(e) The section directory — FPGA-created, driver only walks it.** No kext `STR` targets any
section-directory entry (verified by scanning all stores with bases from self->+0x128/+0x148),
so the Phase 0 `bar_map.txt` IS the live firmware state. Walk algorithm (@0x5d14–0x5e00 and
runtime copy @0x7918–0x7990): entry word W at offset O; type = W & 0xFF (0 terminates); next
offset = O + ((W >> 14) & 0x3FFFC). Live directory:

| Offset | Word | Type | Stride → next |
|---|---|---|---|
| BAR0+0x00000 | 0x04000102 | 2 | +0x1000 |
| BAR0+0x01000 | 0x04000103 | 3 | +0x2000 |
| BAR0+0x02000 | 0x04000104 | 4 (ctrl reg value = 0x100) | +0x3000 |
| BAR0+0x03000 | 0x04000107 | 7 | +0x4000 |
| BAR0+0x04000 | 0x10000106 | 6 | stride 0x4000 → +0x8000 (verified: (0x10000106>>14)&0x3FFFC = 0x4000) |
| BAR0+0x08000 | **0x000b0101** | **1 = DECODER header** | stride decodes to **0** ⚠️ |

⚠️ The type-1 entry's own stride field is 0, so a kext-faithful walk would loop on +0x8000
forever (the loop only exits on type==0). Our driver caps the walk at 16 iterations and stops
at the first type==1 hit — which is all the kext needs from it. Channel blocks occupy
+0x9000…+0xF000; each has header word 0x000b0101, status/ready @+8 (cold: 0x3E0FD000),
counter @+0x14 (=4), per-channel config @+0x24/+0x28.

**(f) Runtime enable function @0x7508–0x7b80 — fully decoded.** Sequence (kext-exact):
1. **Firmware version gate** (@0x75c4–0x78ac): word at BAR0+0x00008 must have bits[31:16]==2
   and bits[15:0] ∈ {0x3E, 0x3F}. Live card reads **0x0002003E → PASSES**. (The probe-time
   copy is additionally gated on bit0 of BAR0+0x2014 — zero at cold boot, so the kext skips
   walk+enable at probe time; the runtime path is what actually enables the decoder.)
2. **Walk** the directory → first type==1 = decoder header (self->+0x158). Not found → log
   "Could not find decoder register section!" @0xb99 and skip enable + init writer entirely.
3. **Disable**: BAR0[hdr+4] = 0, log "(disabling)" with readback; delay ~1ms (bl #0xe89c w0=1).
   **Enable**: BAR0[hdr+4] = 1, log "(enabling)". All 8 channels were disabled at cold boot.
4. **Init writer** @0x5a3c–0x5ad0 (called AFTER enabling):
   - BAR1: +0x90=0xFFFFFE, +0x1090=0xFFFFFE, +0x2010=3, and finally +0x2004=0x16.
   - BAR0 magic region (idempotent on this card — Phase 0 already holds these exact values):
     +0x1040/+0x1048/+0x1050/+0x1064 = 0x2D002A80; +0x104C = 0x2A802800; **+0x1054 =
     0xBCD3BA4D** (magic); +0x1058/+0x1060/+0x1068 = 0x2F802D00; **+0x106C = 0xBF58BCD3**
     (magic).
   - BAR0 words that DO change on this card: +0x1030 and +0x1034: 0x3FFF → **0x7D**.
   - PR block reset: +0x202C = 0 (was 7), +0x2030 = 6 (was 7), +0x2034 = 0.

**(g) Readiness gate — "Decoder not ready!!".** Frame-submit path @~0xa5c0–0xaa80 loads
`[self->+0x158]` (decoder header ptr), reads `[hdr+8]`, and `tbz bit0` → logs "Decoder not
ready!! (0x%08x)" with the raw word. Cold card: 0x3E0FD000, bit0 clear = **not ready**. No
channel-register writes occur in the pre-submit window — enable + init writer is all that's
required before the standard BAR1 doorbell protocol.

**(h) Phase 3k module built.** `afterburner_submit2.c` extended: mode=4 runs the full bring-up
(version gate → walk → disable/enable → init writer → poll bit0 of BAR0[hdr+8] for ~2s, with
before/after readback on every write and channel-block dumps); mode=5 brings up then submits
one descriptor (PA@+0x08) and compares `data(+0x44)` against the 6 from modes 1/3 — if it
changes, enable/readiness was what mattered; if still 6, the payload itself is next. Builds
clean vs the running kernel. **Awaiting live run** (plan.md row 3k).

**(i) Next targets.** (1) Live mode=4: does bit0 of BAR0+0x8008 set after enable + init
writer? If not, chase what else the kext's runtime path does between enable and first frame
(the vtable `readDecodeRegister` entry at ~0x1d7c0–0x1d9a0 is a candidate). (2) Real ProRes
frame payload: generate/obtain a small encoded frame, submit via mode=5-style path, watch the
"PR programming done/error" bit states and frame-number completion. (3) Per-channel config at
[hdr+0x24]/[+0x28] — likely resolution/format selection; decode from the kext's channel-setup
path once a real frame is in flight.

### 21.8 Session addendum — mode=5 live results, @0xa714 byte-verified, ProRes sample format decoded (Phase 3k–3l)

**mode=4/5 live run (results: `/tmp/submit2_result.txt`, mode=5):**
- Decoder was ALREADY enabled when mode=5 ran (`BAR0+0x8004`=1 persisted from the mode=4
  run; card state survives rmmod/insmod). Ready bit `BAR0+0x8008` = **0x3E0FD001** — set,
  so READY immediately (the enable write sets it; cold-boot value was 0x3E0FD000).
- Walk correction: the type-1 entry at +0x8000 has **stride 0x2C**, not 0 as first computed
  (`(0x000b0101>>14)&0x3FFFC = 0x2C`). The walk is well-formed: next word at +0x802C = 0
  (terminator). Driver comment fixed.
- Init writer idempotent as predicted, EXCEPT **`BAR0+0x202C` is not writable** (wrote 0,
  readback stays 7) — treat it as status, not a reset register.
- Submit with decoder READY: completion VALID, `data(+0x44)` = **6 again**, `wordB(+0x48)`=1,
  `BAR0+0x8014` unchanged (4→4). Conclusion: the engine accepts and acks any single-shot
  descriptor regardless of payload; a 64-byte pattern is not being decoded. The interesting
  observables for a real frame are `data(+0x44)`, `wordB(+0x48)` and `BAR0+0x8014`.

**@0xa714 byte-verified (resolves the "generic accessor" doubt):** disassembling past
`dump_sub.txt`'s end shows, inside the frame-submit function (prologue @0xa67c):
```
a6fc: ldr  x8, [x19, #0x158]   ; decoder section header ptr (BAR0+0x8000)
a704: ldr  w27, [x8, #8]       ; ready/status word
a708: tbz  w27, #0, #0xa7b8    ; not-ready -> "Decoder not ready!!" log path
a710: mov  w9, #0x1f
a714: str  w9, [x8, #0x18]     ; BAR0[hdr+0x18] = 0x1F   <- direct inline store
```
So the pre-submit write is real and kext-exact: **when ready, `BAR0[hdr+0x18]` ← 0x1F** goes
out immediately before the submit helper call @0xa28c (which receives an out-param struct at
sp+0x20 initialized to `{[self+0x11e0]>>4, 0}` — consistent with frame-number completion).

**ProRes sample format decoded (from FFmpeg `libavcodec/proresdec.c`):** the raw QuickTime
sample that VideoToolbox hands the kext is:
```
+0x00  u32 BE total sample size (incl. this field)   [our sample: 20138]
+0x04  "icpf" magic                                   (checked by ffmpeg @decode entry)
+0x08  frame header: u16BE hdr_size(=148), u16BE version, "fmpg",
       u16BE width@+8, u16BE height@+10, frame-type/alpha bits@+12..17,
       color info @+14..16, flags@+19 (quant-matrix ptrs), ...
+0x08+hdr_size  picture header: buf[0]>>3 = pic hdr size, u32BE pic_data_size @+1, ...
...            slice index + compressed slices
```
Verified against our extracted sample (`testdata/prores_320x180_frame0.bin`, 20138 B,
sha256 e41a0577…8770d): size field self-consistent, "icpf" present, fmpg box, **320×180
progressive YUV422P10 no-alpha** (buf[12]=0x80 → progressive; alpha_info=0). This is the
payload to DMA for a real decode.

**Per-channel config [hdr+0x24]/[+0x28]:** confirmed across all five remaining store sites in
context — the kext NEVER writes them (firmware-managed); §21.7 next-step (3) is closed as
"not driver-configured". Live values: +0x8024=0x00001201, +0x8028=0x00000020.

**mode=6 built** (`afterburner_submit2.ko`): bring-up + ready poll + the @0xa714 pre-submit
write + single-descriptor submit of the real 20138-byte frame (desc[+4]=20138, PA@+0x08).
New params: `frame=<path>` (loads raw sample into coherent DMA), `poll_ms=` (default 50;
pass 2000 for real frames). Awaiting live run.

**mode=6 first live run (pattern fallback — wrong param name `frame=` instead of
`frame_path=`, file silently not loaded):** bring-up + pre-submit write all clean;
`BAR0+0x8018` was already 0x1F before our write (persists from prior runs / set by fw).
New observation: the disable→enable cycle itself modifies **`BAR0+0x8024`: 0x00000201 →
0x00001201** (bit 0x1000 set) — further evidence +0x24 is firmware-managed, not written by
the kext. data(+0x44)=6 again as expected for the pattern payload. Re-run with `frame_path=`.

### 21.9 Session addendum — wedge resolved, arm64e chain-builder decoded byte-exact (Phase 3m–3o)

**mode=8 live result (wedge RESOLVED):** rung A CONSUMED len=64 immediately after full
bring-up — the decoder disable/enable cycle alone un-wedges the engine. No ack-register
clears, no FLR needed; rungs B/C/D never ran. Writes to `+0x2018`/`+0x2014` do NOT stick
(wrote 0x1, read back 0x3): those registers are **engine-owned**. The only reliable engine
reset is the decoder re-enable cycle (or power cycle).

**Real kext binary located:** `AppleAfterburner.kext/Contents/MacOS/AppleAfterburner` is a
fat Mach-O (big-endian header); arm64e slice at file offset 0x1c000, `__text` vmaddr=0x4000
size=0xa83c. Extracted to scratch/text_arm64e.bin; @0xa714 verified as the direct store of
0x1F (confirms all FINDINGS addresses are VMADDRs and rework/arm64e.bin is a complete slice
with bin offset == vmaddr).

**submitData located at 0xa28c** (not 0x56cc): thin vtable wrapper on `self->+0xa0` — calls
vtable[+0x148](ctx, u32) or vtable[+0x160] depending on a flag; returns the handle. The frame
wrapper (~0xa698–0xa7xx, contains the 0x1F store @0xa714) calls submitData(0xa28c) FIRST and
falls back to fn **@0x9c5c** on failure — primary+fallback, not a sequential two-stage.

**fn @0x9c5c = the real frame path (chain builder).** Prologue arg map: x19=self, x23=IOMemDesc,
x26=self->+0x11d8 (**segment list**), x27=self->+0x11e0>>4 (**segment count N**), x25=user_data
arg (NULL → PA slot +8; non-NULL → +0x10), x20/x24 = two output slots. First action: allocates
N*32 bytes of DMA memory for the descriptor array (`bl #0xe95c` w0=3, w2=0x1000). Then locks,
builds the chain (loop @0xa0f0–0xa164), and commits via `vtable[+0x150](ctx, 1)` on the context
object at self->+0xa8 — the actual staging/doorbell lives inside that vtable method.

**Chain-builder algorithm (byte-exact from @0xa0f0–0xa164):** for N segments, desc i of 32 B:
- `[+0x00]` = `0xAD4B0000 | ((N-2-i) << 8)` for i < N-1; final descriptor = `0xAD4B0013`.
  (Verified against the single-descriptor case: N=1 → exactly 0xAD4B0013, matching Phase 3i.)
- `[+0x04]` = segment length in bytes (raw seg[+8]).
- `[+0x08]` or `[+0x10]` = segment data PA — slot chosen by `cmp w25,#0 / csel eq` on the
  user_data arg.
- `[+0x18]` = **64-bit** store of next descriptor's PA (`str x14,[x12,#0x18]`, x14=head+(i+1)*0x20);
  final descriptor gets 0 (eoc). The post-loop block rewrites the last desc's ctrl to include
  0x13, zeroes its +0x18, and publishes &desc[last].

This RESOLVES the §7.5 discrepancy: that layout (tag at +10) was from a different build/path;
the arm64e single-shot AND chain layouts both put ctrl@+0 / len@+4 — our driver's fill_desc is
kext-exact for N=1, and mode=9 replicates the full algorithm for N>1.

**mode=9 (built):** full bring-up + pre-submit write (hdr+0x18=0x1F) + 4 KB-segment chain of
the frame payload; ladder R9-1 chain→batch bank (+0x5080, ring 0x80FFE7F), R9-2 chain→send bank
(+0x4080, ring 0xFFE7F — tests link-walk on the send path), R9-3 single desc full length → batch
bank; re-bring-up after each failed rung. Completion read kext-handler-style (count==0 → +0x44,
count≥1 → +0x1044). Awaiting live run.

### 21.10 Session addendum — mode=9/10/11 LIVE RESULTS: batch channel accepts chains, send channel does not (Phase 3o–3q)

All three runs used the real ProRes frame (`testdata/prores_320x180_frame0.bin`, 20138 B) after
full kext-exact bring-up. Raw outputs: `/tmp/submit2_result.m9rerun` (Oct 29 07:09),
`/tmp/submit2_result.m10r2` (Oct 29 05:55 — file predates m9rerun but is the mode=10 rerun),
`/tmp/submit2_result.m11` (Oct 29 07:49).

**Chain built by the driver matches the kext byte-exact.** N=5, seg=4096:
ctrl = `0xAD4B0300 / 0xAD4B0200 / 0xAD4B0100 / 0xAD4B0000 / 0xAD4B0013` (final), len@+4,
data PA@+8, next-desc PA@+0x18 (64-bit, final=0). N=10/seg=2048 variant: `…0800 …0700 …0013`.

**mode=9 R9-1 — chain → batch bank (+0x5080), doorbell +0x1004 = 0x80FFE7F: ✅ COMPLETED.**
Staged head PA (0x3c9c9000) into +0x5080, rung the receive/batch doorbell. Completion:
`status(+0x2044)=0x1 VALID`, `count=0`, `data(+0x44)=6` (bit0 clear = normal path),
`wordB(+0x48)=5`; dual ack (+0x2018/+0x2014) cleared status to 0. **The engine consumed the full
5-descriptor chain of a real ProRes frame and completed normally.** This is the first successful
multi-segment submit, and it went through the BATCH/RECEIVE channel — not the send channel.

**mode=10 — kext-exact chain → send bank (+0x4080), doorbell +4 = 0xFFE7F: ❌ NO COMPLETION.**
Pre-submit drain found a pending `status=0x1` (count=0) and handled it (data(+0x44)=6, wordB=5).
After staging the chain head into +0x4080 and ringing 0xFFE7F: status stayed 0 through the whole
poll window; eng-ctrl (+0x2010) stayed 3. During drain handling the driver read decoder status
`BAR0[hdr+0x14]=0x4` — low5=4 is the value for which the kext logs "Error from decoder" (read +
write-back done, per handler semantics).

**mode=11 — instrumented dual chain via send doorbell (+4): ❌ NEITHER COMPLETED.**
Baseline showed a pending batch completion `status=0x2` (**count=1**) → kext branch reads the
R-to-C pair: `data(+0x1044)=6`, `wordB(+0x1048)=5`, batch index from `BAR0[hdr+0x10]>>16 = 0`.
M11-A (N=5/seg 4K) and M11-B (N=10/seg 2K): both rung on +4, **no completion in 30 s each**,
ctr (+0x8014) unchanged at 4.

**Interpretation:**
- The **batch/receive channel** (staging bank `+0x5080..C`, doorbell `+0x1004` = `0x80FFE7F`)
  accepts single descriptors AND full descriptor chains, and completes promptly.
- The **send channel** (bank `+0x4080..C`, doorbell `+4` = `0xFFE7F`) does NOT complete for chain
  submits (mode=10, mode=11-A/B all time out). It may be reserved for a different descriptor
  format/purpose, or require additional setup we haven't replicated.
- Strong implication: the kext's real frame commit (`vtable[+0x150](ctx, 1)` inside fn@0x9c5c)
  stages to the **batch bank and rings 0x80FFE7F** — i.e., R9-1 is likely the kext-exact path.
  Next static target: disassemble the vtable[+0x150] method body (the context object at
  self->+0xa8) to confirm which bank/doorbell it writes.
- The engine still "acks any payload": data(+0x44)=6 regardless of content; wordB=5 on chain runs
  (vs 1 in mode=5 single-shot). No visible ProRes decode output yet — the decoded-frame path
  (where YUV lands) is not identified.
- `BAR0+0x8024` moves 0x201 → 0x1201 on the decoder enable cycle (fw-managed, as before).
  `BAR0+0x8014` is NOT a static counter: it read 0 in m10 pre-state and 4 elsewhere — treat as
  status-ish, not "decoder counter".

**Final card state (mode=0 snapshot, Oct 29 18:39, `/tmp/submit2_result.txt`):** post-bring-up,
engine idle — `+0x2004=0x16`, `+0x2010=3`, acks +0x2014/+0x2018 = 3, completion status 0;
`BAR0+0x8018=0x1F` latched (pre-submit write persists), fw gate word intact (0x0002003E). Card is
healthy and ready for the next run without re-bring-up.

**Ops note:** module params are `mode`, `frame_path`, `data_size`, `poll_ms`. Passing `frame=`
is SILENTLY IGNORED (`unknown parameter 'frame' ignored` in dmesg) and the run falls back to
defaults (mode=0 snapshot). Reloads after first taint produce NO new dmesg lines — check
`/tmp/submit2_result*` mtimes, not dmesg, for load evidence.

## 22. Static RE of the kext binary — vtables, class map, import mechanism (Phase 3p)

Worked on `rework/arm64e.bin` (arm64e slice; original fat kext at
`AppleAfterburner.kext/Contents/MacOS/AppleAfterburner`). All artifacts in **`rework/static_re/`**
(scratch copies were prunable — these are the durable ones): `syms_16.json` (510 arm64e syms),
`syms_x86.json` (503 x86_64 syms), `imports_map.json` (368 imports, dict keyed by string index),
`strtab_1862c.json`, `vtable_named.txt` (full named decode of all vtable entries),
`iouc_slots.json` (37 IOUserClient slots), `chain_main_recount.json` (T=289 chain),
`arm64e_orig.bin` / `x86_64_orig.bin` (extracted slices), XNU headers for Sonoma/Sequoia/main/newsrc.

**Binary facts:** fat binary, big-endian header; arm64e @ file 0x1c000 (0x266b0 B), x86_64 @
0x1000 (0x17b70 B). **NO LC_DYLD_CHAINED_FIXUPS** in either slice. **NO RTTI typeinfo.**

**Import mechanism:** `LC_DYSYMTAB` (cmd=0xb, size 80) at file offset **0x520**, field[28..32)=368;
import-table header @ **0x18000** = {0x0, 0x20, 0x6c, 0x62c, 0x170, 1}; import-name strtab @
**0x1862c** (370 NUL-terminated names). The kext loader patches import references via this table.

**Symbol tables:** arm64e `LC_SYMTAB` symoff=0x1d548, nsyms=**510**, stroff=0x1f6b0,
strsize=0x5668; x86_64 symoff=0xd1b0, nsyms=**503**, stroff=0x10e60, strsize=0x5540. Entry = 16 B:
{u32 strx, u8 type, u8 sect, u16 desc, u32 value, u32 extra}. All 368 import names are present in
the arm64e symtab. **Source files (from symtab): `Athena.cpp` and `AthenaUserClient.cpp`.**

**Vtable entry encoding (CORRECTED — earlier notes said top byte 0x8/0xC, wrong):**
top byte `(v >> 56) == 0x80` → local function pointer, lo32 ∈ [0x4000, 0xE83C) (__text bounds);
`== 0xC0` → import reference, lo32 = index into `syms_16.json`. PAC bits live in the upper 28
positions; always extract via `lo32 = v & 0xFFFFFFFF`.

**Four vtables (arm64e), each with a 2-qword NULL metadata header (no RTTI); vf# = (off − 0x10)/8:**
| Table | Address | Entries | Ends at | Symbol |
|---|---|---|---|---|
| AppleAfterburner main | **0x141e8** | 275 | 0x14a80 | `__ZTV16AppleAfterburner` |
| AppleAfterburner::MetaClass | **0x14a80** | 35 | 0x14b98 | `__ZTVN16AppleAfterburner9MetaClassE` |
| UserClient (DERIVED) main | **0x14b98** | 316 | 0x15578 | — |
| AppleAfterburnerUserClient::MetaClass | **0x15578** | … | — | `__ZTVN26AppleAfterburnerUserClient9MetaClassE` |

Dispatch table @ **0x15680** (16 entries × 24 B) backs the UserClient's externalMethod.
The earlier "310-entry BASE vtable" was an artifact of not recognizing the boundary at 0x14a80.

**x86_64 slice:** main @ **0xb090**, MetaClass @ **0xb8f8** (34 entries), UserClient @ **0xb9f0**
(ends ~0xc3a0). probe() sits at vf#183 vs arm64e's vf#189 — a 6-slot difference attributed to
different XNU header versions per architecture. x86_64 has NULLs where arm64e has local overrides
(probe/start/stop etc.) — unresolved whether that is a build config or loader patching.

**Method identifications (arm64e):**
- fn@0xc998 = **probe()** override — string evidence: "AppleAfterburner Error: %s[%p]::%s: No Device!\n" @0x3069, "probe" @0x3099; super::probe via IOService vtable slot 191 (GOT[0x14148] = `__ZTV9IOService`).
- fn@0x4254 = **start()** override — super-call at slot 192; accesses provider IOPCIDevice at self->+0x88; four provider vtable calls (BAR mapping) at +0x8f8/+0x908/+0x938.
- fn@0xcfc8 = **externalMethod** override (DERIVED vf#272).
- fn@0xcb70 = DERIVED constructor (vptr → 0x14b98+0x10); fn@0xe770 = UserClient real ctor; instance size 6512 B. String "AppleAfterburnerUserClient" @0x309f.
- PLT trampolines at fn@0xe884 → GOT[0x14030]; non-uniform GOT stride (skips 0x50/0x70/0x90).

**Slot counts (public XNU headers, main branch):** OSObject=26, IORegistryEntry=99,
IOService=164, IOUserClient=37 → **T=289**; probe@191 / start@192 / stop@193. The binary implies
the build XNU had 2 fewer pre-probe slots (−2 shift, unresolved). `OSMetaClassDeclareReservedUnused`
padding IS counted (`APPLE_KEXT_VTABLE_PADDING`).

**gMetaClass instances:** @0x10198 (AppleAfterburner) and @0x101c0 (UserClient) — zero-initialized
in the image; set up by static initializers at runtime. metaClass/superClass pointers @
0x141d8/0x141e0 (main) and 0x14b88/0x14b90 (UserClient). hi32 encoding: local refs use
hi32=**0x0010** with lo32 = vmaddr; import refs (superClass) use hi32=**0x4030**.

**UNRESOLVED (carry into next session):**
1. u32 array @ **0x1806c** (368 values): NOT strtab offsets, NOT constant-delta, NOT an
   import-index→symtab mapping (all tested). Possibly LC_DYSYMTAB indirect-symbol related.
2. 97-value array @ **0x1f528**: all valid symtab indices (min=142); purpose unknown.
3. MetaClass vtable layout: Mapping B (sym[142+lo]) shows expected OSMetaClass virtuals at slots
   [19]–[22] but the full 35-entry table doesn't cleanly reconcile with OSMetaClassBase +
   OSMetaClass virtual ordering from `OSMetaClass_sonoma.h`.
4. x86_64 NULL discrepancy (see above) and the −2 slot shift.
5. Dispatch table @0x15680: full selector→function named list for all 37 IOUserClient virtuals not yet produced.
6. `__LINKEDIT` segment layout dump failed once; retry with lief `section.offset`.

**Carried-forward live anchors (unchanged):** submit wrapper fn@0xa67c; pacibsp commit/queue at
0x9698/0x985c; framePath commits via vtable call on object at self->+0xa8; channel object
self->+0x150; FPGA 4KB config write fn@0x6b94 (blob from self->+0x174 → PCI ext-config [0,4096));
flash update fn@0x821c; bitfile load fn@0x42f2 / resource callback fn@0x43fa.

## 23. Vtable/import model — FULLY RESOLVED (Phase 3p, cycles 18–23)

**This section supersedes the "UNRESOLVED" list in §22 items 1–4.** All four are now closed.

### 23.1 Import resolution — FINAL
- `imports_map.json` IS correct and authoritative (cycle-20 "misalignment" was an indexing error).
- True mapping: **GOT slot i → indirect_symtab[43+i] → symtab[symidx] → strtab**. Verified 0/54 mismatches.
- The 97-entry array @0x1f528 = LC_DYSYMTAB `indirect_symbol` table (§22 item 2 CLOSED). Entries 0–42 are unused padding; entries 43–96 map to GOT slots 0–53.
- `imports_map.json` (368 keys) indexes the alphabetically-sorted undefined-symbol list, NOT GOT slots.
- **PLT stubs**: __stubs @0xE83C–0xEA3C, exactly **43 stubs**, each 12 bytes (ADRP #0x14000 + LDR [reg,#off] + BR). `stub_map.json` = complete address→slot map. The 11 slots with NO stub are exactly the data symbols: PAGE_SHIFT_CONST(10), IOPCIDevice::metaClass(14), IOUserClient::gMetaClass(18), IOMapper::gSystem(31), IOService::gMetaClass(34), ZTV_IOUserClient(40), **ZTV_IOService(41)**, stack_chk_guard(44), gIODTModelKey(46), gIODTTypeKey(47), kOSBooleanTrue(48).
- GOT[41] = `__ZTV9IOService` — the kernel's IOService vtable, **raw start** (offset-to-top @+0, typeinfo @+8, first virtual @+16).

### 23.2 The "GOT[52] mystery" — CLOSED
Cycle-20 sites labeled "GOT[52]" were an arithmetic error: offset #0x148 = slot **41**, not 52. All such sites load the IOService vtable base and forward virtual calls to inherited methods via thunks. No mystery object exists.

### 23.3 Vtable entry encoding (PAC-tagged, bits [63:48])
- tag **0x8011** = local function, low32 = vmaddr in __text.
- tag **0xc011** = kernel symbol reference, **low32 = index into the 368-entry undefined-symbol list**.
- tag **0x4010** = on-disk GOT placeholder (in __got only).
- Terminator entries: `0xc041...` after each table.

### 23.4 AppleAfterburner instance vtable — COMPLETE DECODE
- Base **0x141F8**, exactly **271 slots** (0–270), terminator @0x14a70, ends 0x14a78. Full decode: `ab_vtable_true2.txt`.
- Layout = kernel symbols in **reverse-alphabetical order of mangled name** with local overrides inserted at fixed positions. This ordering is the Apple kext convention (deterministic linking); it matches the kernel's IOService vtable layout slot-for-slot, which is why inherited methods need no reordering — only thunks where AB does not override.
- **The +2 rule explained**: GOT[41] points at the RAW table start; instance vptrs point 2 entries in (past offset-to-top + typeinfo). So raw_index = slot + 2. There are NO "extra virtuals" — it is standard Itanium pre-virtual data.
- Local overrides found: [0]/[1] dtor thunks, **[7]=getMetaClass() @0x40ec**, [22] thunk (free path), **[77]=init override @0xc8c0** ✓ (matches earlier model), [132]/[133] thunks → IOSvc 132/133, **[189]=probe() @0xc998**, **[190]=start() @0x4254**, [191]=close? @0xc0a4, [198]/[199] thunks, **[259]=@0x7c60** (setLocation override).
- C2/D2 at slots 28/29 (= virtual indices 28/29) are REAL vtable entries holding IOService::D2/C2 — dispatch targets for the OSMetaClass fVTOffset mechanism (`OSMetaClass.h` `_ptmf2ptf`: byte-offset member pointers, bit0 = virtual flag). NOT a layout anomaly.
- Slot 164 = OSObject::init(); slots 165–180 = _RESERVEDOSObject15..0 (16 reserved); slot 160 = IOService::gMetaClass data ref; slots 162/163 = OSObject operator new/delete.
- Slots 207–238 = 32× _RESERVEDIORegistryEntry*.

### 23.5 Metaclass table @0x14a90 — DECODED (32 slots)
Set as vptr of the gMetaClass object (@0x10198) by ctor fn@0xcacc:
```
OSMetaClassC2(gMetaClass_obj, "AppleAfterburner"@0x6f0, GOT[34]=&IOService::gMetaClass, 0x1310)   ; GOT[15]
vptr = PAC(0x14a90, obj)
```
- [0] @0x4048 = `b GOT[16]` (OSMetaClassD2); [1] @0xcac8 = same (dtor pair).
- **[21] @0x4140 = THE INSTANCE ALLOCATOR** — this is how OSMetaClass::alloc() creates AppleAfterburner objects:
```
buf  = OSObject::operator new(0x1310)        ; GOT[33]
IOService::C2(buf, gMetaClass_obj)           ; GOT[36]
vptr_of_buf = PAC(0x141F8, buf)              ; instance vtable
gMetaClass_obj->instanceConstructed()        ; GOT[38]
return buf
```
- Slots 2–16/22–28 = IORegistryEntry const methods (reverse-alpha); [17]–[20] = OSMetaClass serialize/release×2/retain.
- Two identical C2 wrappers @0x4050/@0x4080: `IOService::C2(x0, meta); vptr=PAC(0x141F8)` — direct-construction helpers (place() path).

### 23.6 Object construction model — COMPLETE
- AppleAfterburner instance = **0x1310 bytes**, allocated via OSObject::operator new, constructed by IOService::C2 (no AB-specific C2), vptr → 0x141F8.
- Destructor: complete dtor @0x40b8 tail-calls GOT[37]=IOServiceD2; deleting dtor @0x40bc calls it then frees size 0x1310. **AB does not override the destructor.**
- UserClient: gMetaClass @0x101c0, ctor fn@0xcb1c/0xe770, instance vtable @0x14b98 (base), metaclass table follows same pattern; descriptor entries at 0x14b88/0x14b90 (hi32=0x0010 local / 0x4030 import encoding confirmed).
- §22 item 3 (MetaClass vtable) CLOSED by this decode. Item 5 (dispatch table @0x15680) and item 6 (__LINKEDIT dump) remain open but low-value.

### 23.7 Remaining static open items (low priority)
- fn@0xc0a4 identity (close? or stop?) — one disassembly away.
- fn@0x7c60 identity (slot 259, setLocation override candidate).
- Dispatch table @0x15680 full named list.
