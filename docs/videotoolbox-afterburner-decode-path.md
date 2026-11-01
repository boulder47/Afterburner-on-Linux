# VideoToolbox ↔ Afterburner ProRes Decode Path — Consolidated Analysis

**Date:** 2026-11-01 · **Phase:** 3r (static RE + third-party VT reference mining)
**Sources:** WWDC20-10090 transcript (`docs/wwdc20-10090-transcript.md`), Apple VideoToolbox docs JSON, FFmpeg `libavcodec/videotoolbox.c`, Streambox sxplayer `decoder_vt.c`, dotnet/macios `VTProfessionalVideoWorkflow.cs`, docs.rs `videotoolbox` crate, and the local kext binary RE artifacts (`rework/arm64e.bin`, `rework/static_re/`).

---

## 1. Executive summary

The Afterburner ProRes decode path is a **two-layer system**:

1. **Public VideoToolbox layer (userspace, OS-owned).** Applications create an ordinary
   `VTDecompressionSession` with hardware acceleration required; professional clients additionally
   call the public C function `VTRegisterProfessionalVideoWorkflowVideoDecoders()` ("Loads decoders
   appropriate for the client's professional video workflows" — Apple docs). **No third-party code
   ever talks to Afterburner directly.** FFmpeg and sxplayer both use only stock VT APIs; macOS
   routes ProRes workloads to the best available engine (software or Afterburner) internally.

2. **Afterburner kext layer (kernel, Apple-owned).** A VideoToolbox *plugin* process holds an IOKit
   connection to `AppleAfterburner` and drives it through exactly **seven user methods** exposed by
   `AppleAfterburnerUserClient::externalMethod`: `submitFrame`, `queryCompletion`, `bindFrame`,
   `cardStatus`, `checkForSpace`, `unbindAll`, `getPercent`. This session fully decoded that ABI:
   the dispatch table, trampoline mechanism, argument-block layout, per-slot buffer model, error
   codes, and the provider-side engine method map (§4–§7).

```
┌─────────────┐  AVFoundation/VT    ┌──────────────────────────┐
│ Application │───────────────────▶│ VideoToolbox (OS)        │
│ (FFmpeg,    │  VTDecompression-  │  session mgmt + engine   │
│ sxplayer…)  │  SessionCreate     │  selection/routing       │
└─────────────┘                    └──────────┬───────────────┘
                                              │ ProRes → Afterburner plugin
                                              ▼
                              ┌──────────────────────────────┐   IOKit IOConnectCallStructMethod
                              │ VT Afterburner client (VT    │──────────────────────────────┐
                              │ plugin process, Apple)       │  selectors 0–6              │
                              └──────────────────────────────┘                             ▼
        ┌─────────────────────────────────────────────────────────────────────────────────────────┐
        │ KERNEL: AppleAfterburnerUserClient::externalMethod                                       │
        │   sMethods @0x15680 (7×24B) → trampoline (IOLock@+0x1960, PAC verify) → real method     │
        │   submitFrame / queryCompletion / bindFrame / cardStatus / checkForSpace / unbindAll /   │
        │   getPercent                                                                              │
        └───────────────┬─────────────────────────────────────────────────────────────────────────┘
                        ▼
        ┌─────────────────────────────────────────────────────────────────────────────────────────┐
        │ KERNEL: AppleAfterburner (provider) engine                                               │
        │   submitFrame(sContext*, inDesc, outDesc, &token, …) → DMA wiring (h2c/c2h)             │
        │   BAR1 doorbell 0x80FFE7F @+0x1004 → FPGA ProRes decoder → decoded YUV DMA'd to          │
        │   user buffer bound via bindFrame; completion polled by queryCompletion                  │
        └─────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. The VideoToolbox side (public API surface)

### 2.1 Session creation — what every client does

FFmpeg (`libavcodec/videotoolbox.c`) and sxplayer both follow the same stock pattern:

- Build a `CMVideoFormatDescription` from the codec parameters. For ProRes, sxplayer maps
  FourCC tags to CM types: `apcn→AppleProRes422`, `apch→AppleProRes422HQ`, `apcs→AppleProRes422LT`,
  `apo→AppleProRes422Proxy`, `ap4x`/`ap4h→AppleProRes4444`.
- Create the session with `VTDecompressionSessionCreate(...)` and a spec dictionary containing:
  - `kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder` (FFmpeg)
  - `kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder` (FFmpeg, when HW is mandatory)
- Register an output callback (`decompressionOutputCallback` in FFmpeg) that receives decoded
  `CMSampleBuffer`s; the client owns buffer release (`videotoolbox_buffer_release`).

**Neither project references Afterburner by name anywhere.** The engine choice is opaque to them —
this confirms the routing decision lives entirely inside macOS.

### 2.2 Professional workflow registration (the Afterburner-relevant API)

Apple's VideoToolbox documentation exposes a dedicated topic section for professional workflows with
two public C functions:

- `VTRegisterProfessionalVideoWorkflowVideoDecoders()` — *"Loads decoders appropriate for the
  client's professional video workflows."*
- `VTRegisterProfessionalVideoWorkflowVideoEncoders()` — encoder counterpart.

dotnet/macios binds this as class `VTProfessionalVideoWorkflow` with static methods
`RegisterVideoDecoders` / `RegisterVideoEncoders`. The docs.rs `videotoolbox` crate (safe Rust
bindings, "hardware-accelerated H.264, HEVC, and ProRes codecs on macOS") lists
`VTProfessionalVideoWorkflow` decoder/encoder registration in its API surface as well.

**Interpretation for our RE:** this is the userspace entry point that makes VideoToolbox load the
Afterburner-backed decode plugin (and its kext client) into a professional session — Final Cut Pro /
Compressor-style clients call it; plain players don't need to, because VT will still route eligible
ProRes work to Afterburner when present.

### 2.3 Output callback contract (what the kext must satisfy)

From FFmpeg's callback handling and Apple docs: the engine delivers decoded frames as
`CMSampleBuffer`s backed by `CVPixelBuffer`s from a client-provided pool; the client releases them
after consumption. For Afterburner this maps to: **decoded YUV lands in user-space buffers that were
pre-bound via `bindFrame`** (see §8), and completion is signalled per-frame through
`queryCompletion`.

---

## 3. Kext UserClient ABI — fully decoded (this session)

All addresses are arm64e `__text`/data offsets in `rework/arm64e.bin` (bin offset == vmaddr).

### 3.1 Class map and lifecycle symbols

From the kext's own symbol strings (`.rodata`, demangled with c++filt):

**`AppleAfterburnerUserClient : IOUserClient`**
| Symbol | Demangled signature |
|---|---|
| `initWithTask` | `(task*, void* cred, uint32_t type, OSDictionary*)` |
| `start` / `stop` | `(IOService*)` |
| `clientDied` / `clientClose` | `()` |
| `didTerminate` / `willTerminate` | `(IOService*, uint32_t, bool*)` |
| `finalize` | `(uint32_t)` |
| `externalMethod` | `(uint32_t selector, IOExternalMethodArguments*, IOExternalMethodDispatch*, OSObject**, void*)` — **the override that implements the 7-method ABI** |
| `sMethods` | static dispatch table (see §3.2) |
| `submitFrame` | `(s_inputDataStruct*, uint64_t* tokenOut)` |
| `queryCompletion` | `(uint64_t* frameNum, uint64_t* statusOut)` |
| `bindFrame` | `(s_bindDataStruct*, uint64_t*)` |
| `cardStatus` | `(uint64_t*)` |
| `checkForSpace` | `(s_bufDataStruct*)` |
| `unbindAll` | `()` |
| `getPercent` | `(uint64_t*)` |
| `sSubmitFrame`, `sQueryCompletion`, `sBindFrame`, `sCardStatus`, `sCheckForSpace`, `sUnbindAll`, `sGetPercent` | trampolines, each `(OSObject* target, void*, IOExternalMethodArguments*)` |

Note the second argument of `submitFrame`/`bindFrame` is a **pointer** (`u64*`), not a scalar —
confirmed by c++filt on the kext's own mangled names.

### 3.2 The dispatch table: `sMethods` @0x15680 (resolves plan.md §7 item 4 / FINDINGS §23.7)

`externalMethod` @0xcfc8 disassembles to: if `selector < 7`, replace the dispatch pointer with
`&sMethods[selector]` and tail-call the inherited `IOUserClient::externalMethod` (base vtable
offset +0x890); otherwise pass through unchanged. The table holds **seven 24-byte records**:

| sel | method | trampoline fn | A @+8 | B @+12 | C @+16 | D @+20 |
|---|---|---|---|---|---|---|
| 0 | submitFrame | `sSubmitFrame` @0xcd7c | 0 | **0x28 (40)** | 1 | 0 |
| 1 | queryCompletion | `sQueryCompletion` @0xcdf8 | **1** | 0 | 1 | 0 |
| 2 | bindFrame | `sBindFrame` @0xce34 | 0 | **0x20 (32)** | 2 | 0 |
| 3 | cardStatus | `sCardStatus` @0xceb0 | 0 | 0 | 1 | 0 |
| 4 | checkForSpace | `sCheckForSpace` @0xcee8 | 0 | **0x10 (16)** | 0 | 0 |
| 5 | unbindAll | `sUnbindAll` @0xcf20 | 0 | 0 | 0 | 0 |
| 6 | getPercent | `sGetPercent` @0xcf90 (tag 0x8000) | 0 | 0 | 1 | 0 |

Field model — **consistent with all seven records**:
- **B = input structure size in bytes.** Matches the struct names exactly:
  `s_inputDataStruct`=40, `s_bindDataStruct`=32, `s_bufDataStruct`=16; scalar/void methods have B=0.
- **A = number of scalar u64 inputs** (delivered at argument-block offset +0x20). Only
  queryCompletion has A=1 (its frame-number input).
- **C = output size in qwords.** submitFrame/queryCompletion/cardStatus/getPercent write one qword;
  bindFrame two; checkForSpace/unbindAll zero.

### 3.3 Trampoline mechanism (all seven disassembled)

Each trampoline `sXxx(target, ctx, args)` does:

1. `IOLockLock(*(target->+0x1960))` — **one IOLock per UserClient instance** guards the whole
   frame-dispatch path (constant offset in all seven trampolines).
2. Read inputs from a **custom argument block** (not the stock 0x20-byte `IOExternalMethodArguments`):
   - scalar u64 input @ **+0x20** (queryCompletion only)
   - struct pointer @ **+0x30** (`s_inputDataStruct` / `s_bindDataStruct` / `s_bufDataStruct`)
   - output pointer @ **+0x48** (the `u64*` out parameter of each method)
3. **PAC-verify the target's vptr**: load `[target]`, `autda` with auth data = the object pointer
   itself, tag bits 0xcda1; mismatch → `brk #0xc472`. (Same scheme used for UC's own vptr and for
   per-slot sub-objects — see §5.)
4. Tail-call (`braa`) the real virtual method at `[vptr + off]`:

| trampoline | range | calls [vptr+…] | raw vtable slot | fn addr |
|---|---|---|---|---|
| sSubmitFrame | 0xcd7c–0xcdf8 | +0x990 | **308** | 0xd708 |
| sQueryCompletion | 0xcdf8–0xce34 | +0x998 | **309** | 0xdcc4 |
| sBindFrame | 0xce34–0xceb0 | +0x9a0 | **310** | 0xdd9c |
| sCardStatus | 0xceb0–0xcee8 | +0x9a8 | **311** | 0xe2a4 |
| sCheckForSpace | 0xcee8–0xcf20 | +0x9b0 | **312** | 0xe36c |
| sUnbindAll | 0xcf20–0xcf90 | +0x9b8 | **313** | 0xe4c0 |
| sGetPercent | 0xcf90–0xcfc8 | +0x9c0 | **314** (tag 0x8041) | 0xe6a4 |

### 3.4 Vtable tail — structural confirmation

UC vtable base = 0x14b98. `UC::C2` @0xcd24 sets the live object's vptr to **&vtable[2]** (past the
two Itanium pre-virtual entries), so `[vptr+0x990]` == raw slot 308, etc. The tail of the table:

```
raw slot  addr     value              resolved
   304    0x15518  c011:import        IOUserClient::getTargetAndMethodForIndex(OSObject**, uint32_t)
   305    0x15520  c011:import        IOUserClient::getAsyncTargetAndMethodForIndex(...)
   306    0x15528  c011:import        IOUserClient::getExternalTrapForIndex(uint32_t)
   307    0x15530  c011:import        IOUserClient::getTargetAndTrapForIndex(...)
   308    0x15538  8011:local         submitFrame      @0xd708   ← first of the seven
   …
   314    0x15568  8041:local         getPercent       @0xe6a4   ← last; slot 315 = NULL terminator
```

The seven frame methods are **appended at the very end of the vtable**, immediately after the four
inherited `IOUserClient` external-method virtuals (the kernel's selector-dispatch machinery). This
is why live dispatch lands in raw slots 308–314 (= indices 306–312 relative to the live vptr start)
— and it retroactively explains the earlier "slot 306 = getExternalTrapForIndex" confusion: that was
an off-by-two indexing error of the same class as the GOT[52] incident (FINDINGS §23.2).

### 3.5 PLT / import map (all 43 stubs resolved)

PLT block @0xE83C–0xEA3C, 12-byte stubs; each loads a tag-0x4010 GOT entry whose low 32 bits index
the 368-entry kernel-import list (`rework/static_re/imports_map.json`). Notable entries used by the
frame path:

| stub | addr | import |
|---|---|---|
| 4 / 5 | 0xe86c / 0xe878 | `_IOLockLock` / `_IOLockUnlock` |
| 6 | 0xe884 | `_IOLog` |
| 21 | 0xe938 | `IOMemoryDescriptor::withAddressRange(vm_address_t, vm_size_t, IOOptionBits, task_t)` |
| 33 / 34 | 0xe9c8 / 0xe9d4 | `OSMetaClassBase::instanceConstructed` / `IOService::isInactive()` |
| 38–42 | 0xea04…0xea34 | `_kernel_debug`, `_microtime`, `_snprintf`, `_strlen`, `_timevalsub` |

---

## 4. `submitFrame` — complete control flow (fn @0xd708)

```
UC::submitFrame(s_inputDataStruct* in, u64* tokenOut):
    base = 0xE000_02C2                          ; IOReturn error base for this method
    engine = UC->+0xd8                           ; provider context object (sContext*)
    if (!engine || IOService::isInactive(engine)):
        [uc_vt+0x3a8](this, 0)                   ; virtual reset/clear on the UC itself
        IOLog("AppleAfterburner Error: %s[%p]::%s: no provider or inactive\n", …, "submitFrame")
        return base + 0x17                       ; = 0xE000_02D9
    if (!in || !in[+0] || !in[+8]):              ; null buffer object / zero length
        IOLog("…bad argument %p %llu %lu %llu %u %p\n")   ; @0x3206
        return base + …                          ; non-zero failure

    slotIdx = in[+0x10]
    if (slotIdx >= UC->+0x18f0): fail            ; N = UC->+0x18f0 = per-slot count
    slotObj = *(UC + 0xf0 + slotIdx*0x18)        ; array of per-slot sub-object pointers, stride 0x18
    if (!slotObj): fail
    PAC-verify(slotObj.vptr)                     ; auth data = slotObj pointer, tag 0xcda1
    r = [slot_vt+0x1e8](slotObj)                 ; virtual "prepare/validate" — must return exactly 1
    if (r != 1): fail

    {a, b} = *(UC + 0xf8 + slotIdx*0x18)         ; per-slot DMA qword pair (addr/size or PA lo/hi)
    kernel_debug(0xFA4_2B61, in[+8]+4, 4, 0,0,0) ; trace: frame length (+4 header?)
    kernel_debug(0xFA4_2B61, b,       5, 0,0,0) ; trace: second DMA qword

    argblock @stack = { x28, in[+8](len), a, b } ; + params w5=in[+0x18], w6=in[+0x1c], w7=in[+0x20]
    status = fn@0xa9ac(engine, &argblock, bufObj=in[+0], slotObj, tokenOut)
             ; == AppleAfterburner::submitFrame(sContext*, IOMemoryDescriptor* in,
             ;                                  IOMemoryDescriptor* out, u64* token,
             ;                                  u32, u32, u32, sDescriptorLengthOffsetInfo)

    kernel_debug(0xFA4_2B61, *tokenOut, 5, …)
    switch (status):
      case 0:            // ACCEPTED
          UC->+0x1958 = *tokenOut                ; store completion token
          [slot_vt+0x1f8](slotObj, 1)            ; virtual "mark submitted"
          return kIOReturnSuccess (0)
      case 0xE000_02D8:  // NO SPACE — engine rejected
          PAC-verify(bufObj.vptr)
          [buf_vt+0x220](bufObj, 0)              ; release/reset the input buffer object
          [buf_vt+0x0](bufObj)                   ; second virtual (free/teardown)
          return base + 0x16                     ; = 0xE000_02D8 (passed through verbatim)
      default:           // other failure
          IOLog("…Could not submit frame: %u!\n")   ; @0x3325
          return non-zero
```

**`s_inputDataStruct` (40 bytes, from dispatch record B=0x28):**
```c
struct s_inputDataStruct {
    void *bufObj;      // +0x00  PAC-protected buffer descriptor object (NOT a raw address)
    u64   length;      // +0x08  frame data length
    u32   slotIdx;     // +0x10  per-slot index, bounds-checked against UC->+0x18f0
    u32   p1, p2;      // +0x18  two u32 params forwarded to the engine
    u32   p3;          // +0x20  one more u32 param forwarded to the engine
    /* …padded to 40B… */
};
```

**UC instance layout (frame-path-relevant offsets):**
| offset | meaning |
|---|---|
| `+0xd8` | provider/engine context object (`sContext*`) — null ⇒ "no provider or inactive" |
| `+0xf0 + i·0x18` | per-slot sub-object pointer, slot *i* (N slots) |
| `+0xf8 + i·0x18` | per-slot DMA qword pair `{a, b}` |
| `+0x18f0` | N — number of per-slot entries (bounds check in submitFrame; bindFrame requires N > 255 on one path) |
| `+0x18f8` | status/result field passed by pointer into the engine call |
| `+0x1958` | last completion token (written on accept, read back for tracing) |
| `+0x1960` | `IOLock*` guarding all seven frame-method trampolines |

**Error codes:** base `0xE000_02C2`; `+0x17 = 0xE000_02D9` (no provider / inactive);
engine-returned `0xE000_02D8` = no space (input buffer released, code passed through).

---

## 5. The other six selectors

### bindFrame (fn @0xdd9c) — pre-submission buffer binding
```
slotIdx = in[+0x18]; stride = in[+0x10] (u8, userspace-supplied; submitFrame hardcodes 0x18)
slotObj = *(this + slotIdx*stride + 0xf0); PAC-verify(slotObj.vptr)
r = [slot_vt+0x200](slotObj)          ; virtual "bind" — must return exactly 1
```
Log evidence: `"bad argument (frame in use)"` @0x33e9 → re-binding an already-bound frame is
rejected. Provider side: `AppleAfterburner::bindFrame(IOMemoryDescriptor*)`.

### queryCompletion (fn @0xdcc4) — completion polling with hang detection
UC signature `(u64* frameNum, u64* statusOut)`; provider side
`queryCompletion(sContext*, u64*, u64*)`. Log evidence from the engine:
- `"Possible hang, frameNum %llu, completionStamp %lld"` @0x2367 — timeout watchdog
- `"Query failed (resetting), … _numFrames %u, status 0x%08x"` @0x2a91 and
  `"Query timed out (resetting), …"` @0x2b27 — on failure the engine **resets itself**

### checkForSpace / freeSpace
`checkForSpace(s_bufDataStruct*)` → provider `checkForSpace(u64*)`; companion
`freeSpace(u64)` exists in the provider symbol table. This is the back-pressure API a VT client
uses before submitting (mirrors `kVTDecodeFrameASAP`-style flow control).

### cardStatus / getPercent / unbindAll
`cardStatus(u64*)` and `getPercent(u64*)` are telemetry reads; `unbindAll()` tears down all slot
bindings (provider has per-buffer `unbindFrame(IOMemoryDescriptor*)`). Called on session teardown —
the UC also implements `clientDied()`/`clientClose()` for the IOKit connection lifecycle.

---

## 6. Provider engine map (`AppleAfterburner`, demangled)

Complete method inventory from the kext symbol strings, grouped by function:

**Frame path (called by the UC):**
- `submitFrame(sContext*, IOMemoryDescriptor* in, IOMemoryDescriptor* out, u64* token, u32, u32, u32, sDescriptorLengthOffsetInfo)` — **the real frame commit; takes both an input and an output memory descriptor** (§8)
- `submitData(sContext*, IOMemDesc*, u32, u64*, u32, u32, u32, u64, u64)` + `submitDataGated(sDmaConfig*)` — low-level DMA send
- `queryCompletion(sContext*, u64*, u64*)` / `queryCompletionGated(u64*, int*)`
- `bindFrame(IOMemDesc*)`, `unbindFrame(IOMemDesc*)`, `checkForSpace(u64*)`, `freeSpace(u64)`
- `getSegments(sContext*, IOMemDesc*, Segment64**, u32*, u64, u64, u64)`,
  `getDescriptorList(sContext*, Segment64**, u32, u32, u64*, u32*, u64)` — DMA segment enumeration

**DMA channel health (log evidence):** `"c2hStatus 0x%08x"` / `"h2cStatus 0x%08x"`,
`"failed to submit send data: %u"` / `"failed to submit receive data: %u"`,
`"No entry (e = …) in dmaQ - potential leak"` / `…descDmaQ…` — two DMA command queues
(host→card, card→host), matching the BAR1 send (+0x4080/doorbell +4) and receive/batch
(+0x5080/doorbell +0x1004) channels we verified live in Phases 3i–3o.

**Decoder control & error handling:** `"Decoder Control Register: 0x%08x (enabling/disabling)"`,
`"Decoder not ready!! (0x%08x)"`, `"Error from decoder / HARD error from decoder"`,
`"Offending frameNum: 0x%016llx"`, `"Completion for frameNum %llu, errFrameNum %llu"`,
`"Decoder error (resetting) _frameNum … _pendingData %u"` — the engine tracks per-frame numbers and
completion stamps, resets on hard errors.

**Thermal:** six ADC channels logged (backcard, FPGA die, faceplate, 0V85 VRM, bottom, sysmon);
`"dropping frame due to thermal distress condition 0x%08x"` — **frames are dropped under thermal
distress**, which a VT client would see as missing completions.

**Flash / bitfile state machine:** image types golden/live/HWTE/debug; full SPI programming logs
(`wr fifo` waits, erase/program progress), `"PR programming error! / done!"`, pointer switching
between golden and live images — the partial-reconfiguration path (Phase 5 material).

**Register accessors (all present):** BAR0 read/write, DMA register read/write, decode register
read/write, flash read/write, bitfile read/write, thermal read, version read/write.

---

## 7. Correlation table: VideoToolbox concept → kext mechanism

| VT / client-side concept | Afterburner kext mechanism (verified) |
|---|---|
| `VTRegisterProfessionalVideoWorkflowVideoDecoders()` | loads the Afterburner VT plugin + its IOKit connection (`IOService::newUserClient` → `AppleAfterburnerUserClient::initWithTask`) |
| Output buffer pool / CVPixelBuffers | per-slot sub-objects; buffers bound via **bindFrame** (sel 2) before submission; "frame in use" guard |
| `VTDecompressionSessionDecodeFrame(sample)` | **submitFrame** (sel 0): `s_inputDataStruct{bufObj, len, slotIdx, p1..p3}` + token-out |
| Frame completion callback / status | **queryCompletion** (sel 1) with hang watchdog ("Possible hang") and self-reset on timeout |
| Back-pressure / space queries | **checkForSpace** (sel 4) + provider `freeSpace` |
| Telemetry (progress, card health) | **getPercent** (sel 6), **cardStatus** (sel 3) |
| Session teardown | **unbindAll** (sel 5); IOKit side: `clientDied`/`clientClose` → UC `stop` |
| Decoded YUV delivery | output `IOMemoryDescriptor` in provider `submitFrame`; c2h DMA writes into the bound user buffer (§8) |

---

## 8. The decoded-output path (resolves plan.md §7 item 2)

Evidence assembled this session:

1. Provider `AppleAfterburner::submitFrame` takes **two** `IOMemoryDescriptor`s — input *and*
   output (c++filt-verified signature).
2. UC-side log strings `"failed to get input memory descriptor"` / `"failed to get output memory
   descriptor"` / `"failed to wire memory: %u"` sit in the UC error-string block (@0x324f–0x32d9) —
   the UC wires both descriptors before calling the engine.
3. The per-slot DMA qword pair at `UC+0xf8+i·0x18` is read during submitFrame and forwarded into the
   engine call's argument block — it describes where the output lands.
4. `bindFrame` exists precisely to attach a user buffer (as an `IOMemoryDescriptor`) to a slot, with
   a "frame in use" guard; `unbindFrame`/`unbindAll` detach them.

**Conclusion:** decoded ProRes YUV is DMA'd **card→host directly into the user-space buffer that was
bound to the slot via bindFrame** (c2h channel — BAR1 +0x5080 staging / doorbell +0x1004, per our
live Phase 3o verification). The client then learns of completion by polling `queryCompletion` with
the frame number/token issued at submit time. There is no intermediate kernel buffer copy on the
happy path — the "output memory descriptor" *is* the user's pixel buffer (or its DMA mapping).

---

## 9. Open items (updated)

Resolved this session: dispatch table @0x15680 named list (plan.md §7 item 4, FINDINGS §23.7);
decoded-output path (plan.md §7 item 2); trampoline→vtable slot mapping incl. the Itanium +2 offset.

Still open:
- `fn@0xc0a4` identity (close? or stop?) and `fn@0x7c60` (slot 259) — one disassembly each (FINDINGS §23.7).
- Exact field semantics of `sDescriptorLengthOffsetInfo` (passed by value into provider submitFrame;
  the stack argblock in UC submitFrame is its likely source).
- Why bindFrame takes the slot stride from userspace (`in[+0x10]`) while submitFrame hardcodes 0x18 —
  probably defensive/legacy; live traffic capture on macOS would settle it.
- `s_bufDataStruct` (16B) field layout beyond "carries a size" — checkForSpace body not yet fully walked.
- Live confirmation of the whole ABI: dtrace/IOKit-trace a real Final Cut Pro session on macOS to
  capture actual selector traffic and struct contents.
