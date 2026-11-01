# WWDC20-10090 — Decode ProRes with AVFoundation and VideoToolbox

Source: https://developer.apple.com/videos/play/wwdc2020/10090/
Captured from the session page's transcript chapter links (Apple does not publish a
standalone transcript file). ~23 min talk.

## Why this matters for afterburner-re

This is Apple's developer-facing description of how Afterburner cards plug into the
macOS ProRes decode pipeline. Key facts extracted:

1. **Registration API**: `VTRegisterProfessionalVideoWorkflowVideoDecoders()` — apps call
   this once to "access the set of specialized decoders distributed for Pro video
   workflows". This is the userspace entry point that makes VT aware of the
   Afterburner-backed ProRes decoder (the kext's counterpart).
2. **HW decode is ON by default** on current OS versions, explicitly including Afterburner:
   "all media interfaces will automatically enable hardware decode when available.
   This includes enabling Afterburner."
3. **Decoder selection keys** (videoDecoderSpecification dict for VTDecompressionSessionCreate):
   - `kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder` = true → session creation fails if no HW decoder
   - `kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder` = false → force software decode
4. **Process model**: VideoToolbox runs decoders OUT-OF-PROCESS in a sandboxed server;
   CMSampleBuffers from AVFoundation are "optimized for transfer over the RPC boundary"
   (i.e. shared-memory/IOSurface-backed block buffers cross the XPC line). A decoder crash
   surfaces as a decode error, not an app crash.
5. **Output path**: decoded frames = CVPixelBuffer wrapping IOSurface, allocated from the
   session's CVPixelBufferPool; VTPixelTransferSession converts if requested format differs.
   Native (no-conversion) formats: ProRes 422 → `v216` (16-bit 4:2:2 YCbCr),
   ProRes 4444 → `y416` (16-bit 4:4:4:4 + alpha), ProRes RAW → `RGhA` (RGBA half-float).
   => card's native output is 16-bit YUV, consistent with our YUV422P10 fixture.
6. **Decoder output is serialized**: one frame out at a time; blocking in the output handler
   creates back-pressure through the decoder — matches the sequential descriptor-chain
   behavior we observed on BAR1 (engine consumes chains one at a time).

## RE implications / next steps

- ~~Grep AppleAfterburner kext strings for IOSurface / XPC / mach-service symbols to find how
  decoded DMA buffers are handed back as IOSurfaces~~ — **RESOLVED (2026-11-01)**: the provider's
  `submitFrame` takes an *output* IOMemoryDescriptor; decoded YUV is c2h-DMA'd directly into the
  user buffer bound via `bindFrame`. See consolidated analysis below.
- ~~On macOS, locate the plugin/bundle that VTRegisterProfessionalVideoWorkflowVideoDecoders()
  loads — it will show exactly which kext methods get called per frame~~ — **RESOLVED (2026-11-01)**:
  the kext exposes exactly seven user methods (`submitFrame`, `queryCompletion`, `bindFrame`,
  `cardStatus`, `checkForSpace`, `unbindAll`, `getPercent`); full ABI decoded. See below.
- The v216/y416/RGhA native formats tell us what pixel layout to expect in the output DMA
  buffer when we finally capture decoded YUV from the card.

---

## Consolidated analysis (2026-11-01) → `videotoolbox-afterburner-decode-path.md`

Full write-up: **`docs/videotoolbox-afterburner-decode-path.md`** — ties this talk to the
third-party VideoToolbox references (FFmpeg, sxplayer, dotnet/macios, docs.rs, Apple docs) and
the local kext RE. Headline results:

1. **Clients never touch Afterburner directly.** FFmpeg/sxplayer use only stock VT APIs; macOS
   routes ProRes to the best engine internally. `VTRegisterProfessionalVideoWorkflowVideoDecoders()`
   (Apple public C API, bound in dotnet/macios as `VTProfessionalVideoWorkflow.RegisterVideoDecoders`)
   is what makes professional sessions load the Afterburner-backed plugin.
2. **The kext UserClient ABI is fully decoded**: `externalMethod` override @0xcfc8 dispatches via a
   7×24-byte table at **@0x15680** (resolves plan.md §7 item 4 / FINDINGS §23.7). Record fields:
   B = input struct size in bytes (`s_inputDataStruct`=40, `s_bindDataStruct`=32,
   `s_bufDataStruct`=16), A = scalar u64 inputs, C = output qwords. Trampolines lock
   `IOLock@UC+0x1960`, PAC-verify the target vptr (auth data = object ptr, tag 0xcda1), read a
   custom arg block (scalar @+0x20, struct @+0x30, out @+0x48) and tail-call raw vtable slots
   **308–314** — the seven frame methods appended at the very end of the UC vtable, right after
   the four inherited `IOUserClient` external-method virtuals (the earlier "slot 306 =
   getExternalTrapForIndex" was an off-by-two from the Itanium pre-virtual entries).
3. **submitFrame flow**: bounds-check slot index vs `UC->+0x18f0`, fetch per-slot sub-object at
   `UC+0xf0+i·0x18` (PAC-verified), read DMA qword pair at `UC+0xf8+i·0x18`, call the provider's
   `submitFrame(sContext*, inDesc, outDesc, &token, u32×3, sDescriptorLengthOffsetInfo)`.
   Outcomes: accept (store token @`UC->+0x1958`) / no-space `0xE000_02D8` (release input buffer
   object, pass code through) / other failure (IOLog "Could not submit frame"). Error base
   `0xE000_02C2`; no-provider/inactive = `+0x17`.
4. **Engine behavior from log strings**: per-frame numbers + completion stamps with a hang
   watchdog ("Possible hang, frameNum %llu"), self-reset on query timeout, decoder enable/disable
   via the Decoder Control Register, hard-error reset paths, and **frame drops under thermal
   distress** (six ADC channels monitored). DMA health: h2c/c2h status + dmaQ/descDmaQ leak
   detection — matching our live BAR1 send/receive channel findings.

---

## Transcript (from page chapter links)


