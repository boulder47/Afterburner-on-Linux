# Afterburner UC Interface — Complete Method Map (Linux Driver Reference)

Living document: every decoded method of the AppleAfterburner kext's user-client layer,
with exact addresses, offsets, ABIs and error codes. **Append each new decode here as it
is completed** so work survives context compaction. Binary: `rework/arm64e.bin` (arm64e
slice; file offset == vmaddr for __text 0x4000–0xE83C).

Legend: ✅ fully decoded · 🟡 partially decoded · ⬜ not yet decoded

---

## 1. Dispatch architecture (userland → card)

```
userland IOUserClient::method(selector, args...)
   └─> UC vtable slot 274 (+0x890): externalMethod @0xcfc8        [shared dispatch tail / thin dispatcher]
         ├─ known selector → sMethods record (table @0x15680)
         │     └─> trampoline s* @0xcd7c..0xcf98
         │           1. IOLockLock(UC+0x1960)
         │           2. PAC-verify vptr (tag 0xcda1) — brk #0xc472 on mismatch
         │           3. blraa runtime-vtable-slot → method body
         └─ unknown selector → base IOUserClient implementation (GOT[41] = IOService vtable)
```

- **Trampoline arg block** (verified against every s* trampoline): scalars @ args+0x20,
  input struct ptr @ args+0x30, output buffer ptr @ args+0x48.
- Trampolines: sSubmitFrame@0xcd7c · sQueryCompletion@0xcdf8 · sBindFrame@0xce34 ·
  sCardStatus@0xceb0 · sCheckForSpace@0xcee8 · sUnbindAll@0xcf20 · sGetPercent@0xcf90.
  (disasm: `tmp/dis_uc_trampolines.txt`)

## 2. UC vtable — all LOCAL overrides ✅

`__ZTV26AppleAfterburnerUserClient` @ **0x14b98** (pre-virtual at +0/+8; raw slot i =
offset +i·8). 17 LOCAL (tag 0x80) overrides:

| raw slot | offset | addr | demangled signature | status |
|---|---|---|---|---|
| 2   | +0x10  | 0xcbe0 | `~AppleAfterburnerUserClient() [D1Ev]`            | ✅ §19.1 (tail-call base D2; no local cleanup) |
| 3   | +0x18  | 0xcbe4 | `~AppleAfterburnerUserClient() [D0Ev]` (deleting) | ✅ §19.1 (base D2 + `operator delete(this, 0x1970)`) |
| 9   | +0x48  | 0xcc14 | `getMetaClass()`                                   | ✅ §19.1 (returns &0x101C0) |
| 134 | +0x430 | 0xd54c | `willTerminate(IOService*, u32)`                   | ✅ §19.5 (log + tail-call base slot 134) |
| 135 | +0x438 | 0xd60c | `didTerminate(IOService*, u32, bool*)`             | ✅ §19.5 (log; **\*relinq=false**; tail-call base slot 135) |
| 192 | +0x600 | 0xd1e4 | `start(IOService*)`                                | ✅ §19.2 (safeMetaCast → UC+0xD8; no HW work) |
| 193 | +0x608 | 0xd25c | `stop(IOService*)`                                 | ✅ §19.3 (cleanUp + slot-release loop + freeSpace + 5×IOLockFree; writes UC+0x1968 @0xd2f0) |
| 200 | +0x640 | 0xd6d8 | `terminate(u32)`                                   | ✅ §19.5 (pure tail-call base slot 200) |
| 201 | +0x648 | 0xd6f0 | `finalize(u32)`                                    | ✅ §19.5 (pure tail-call base slot 201) |
| 274 | +0x890 | 0xcfc8 | `externalMethod(...)` — dispatch tail              | ✅ (`tmp/dis_uc_externalMethod.txt`) |
| 292 | +0x920 | 0xd004 | `initWithTask(task*, void*, u32, OSDictionary*)`   | ✅ (`tmp/dis_uc_initWithTask.txt`) |
| 294 | +0x930 | 0xd3f8 | `clientClose()`                                    | ✅ §19.4 (no-provider → 0xE000_02D9; base handleClose hook) |
| 295 | +0x938 | 0xd534 | `clientDied()`                                     | ✅ §19.4 (pure tail-call base slot 295) |
| 308 | +0x990 | 0xd708 | `submitFrame(s_inputDataStruct*, u64*)`            | ✅ §4.1 |
| 309 | +0x998 | 0xdcc4 | `queryCompletion(u64, u64*)`                       | ✅ §4.2 |
| 310 | +0x9a0 | 0xdd9c | `bindFrame(s_bindDataStruct*, u64*)`               | ✅ §4.3 |
| 311 | +0x9a8 | 0xe2a4 | `cardStatus(u64*)`                                 | ✅ §4.4 |
| 312 | +0x9b0 | 0xe36c | `checkForSpace(s_bufDataStruct*)`                  | ✅ §4.5 |
| 313 | +0x9b8 | 0xe4c0 | `unbindAll()`                                      | ✅ §4.6 |
| 314 | +0x9c0 | 0xe6a4 | `getPercent(u64*)`                                 | ✅ §4.7 |

Slots 306/307 = inherited IOKit virtuals (KSYM tag 0xC0). Slot 117 (+0x3a8) =
IORegistryEntry::getName — used by every error-log path as `getName(UC, 0)`.
All base-class tail calls go through **GOT[28] = pointer to the base IOUserClient
vtable** (verified: [GOT28+0x430/0x438/0x600/0x608/0x630/0x640/0x648] resolve to
base slots 134/135/192/193/198/200/201; slot 198 = `IOService::handleClose`,
KSYM undef[127]). GOT[57] = &AB gMetaClass data pointer (safeMetaCast in start()).

## 3. sMethods table @0x15680 ✅ (the ioctl ABI contract)

7 records × 24 bytes. Field layout **verified empirically against trampoline consumption**:
`{ u64 function; u32 inScalars@+8; u32 inStructSize@+0xC; u32 outScalars@+0x10; u32 outStructSize@+0x14 }`

| selector | method        | trampoline | inScalars | inStruct | outScalars | outStruct |
|---|---|---|---|---|---|---|
| 0 | submitFrame     | 0xcd7c | 0 | **40** (s_inputDataStruct) | 1 | 0 |
| 1 | queryCompletion | 0xcdf8 | **1** | 0 | 1 | 0 |
| 2 | bindFrame       | 0xce34 | 0 | **32** (s_bindDataStruct)  | **2** | 0 |
| 3 | cardStatus      | 0xceb0 | 0 | 0 | 1 | 0 |
| 4 | checkForSpace   | 0xcee8 | 0 | **16** (s_bufDataStruct)   | 0 | 0 |
| 5 | unbindAll       | 0xcf20 | 0 | 0 | 0 | 0 |
| 6 | getPercent      | 0xcf90 | 0 | 0 | 1 | 0 |

### Input struct layouts (from body field loads)

```c
// s_inputDataStruct — 40 bytes (submitFrame)
struct s_inputDataStruct {
    u64 ptr;        // +0x00  input frame buffer PA (ProRes data); NULL/0 → badarg
    u64 size;       // +0x08  byte length; 0 → badarg
    u32 slot_idx;   // +0x10  output-slot index; must be < UC+0x18F0 (slot count)
    u32 ?;          // +0x14
    u64 ?;          // +0x18
    u32 flag;       // +0x20  passed as w7 to AB::submitFrame (stage/flag select)
    ...            // 40B total
};

// s_bindDataStruct — 32 bytes (bindFrame)
struct s_bindDataStruct {
    u64 ptr;        // +0x00  output buffer PA (decoded pixels land here); NULL → badarg
    u64 size;       // +0x08  byte length; 0 → badarg
    u8  flag;       // +0x10  dumped in debug log @0x339A — semantics TODO
    u64 field;      // +0x18  dumped in debug log — possibly explicit slot index (TODO)
};

// s_bufDataStruct — 16 bytes (checkForSpace)
struct s_bufDataStruct {
    u64 ptr;        // +0x00  buffer PA (used for alignment base); NULL/0 → badarg
    u64 size;       // +0x08  byte length; 0 → badarg
};
```

## 4. Method details (the seven frame methods)

Common wrapper pattern in every body:
1. `w19 = 0xE000_02D9` default return ("no provider / inactive")
2. `AB = [UC+0xD8]`; if NULL → IOLog + return w19
3. `isInactive()` (IOService vcall) → return w19
4. arg validation; badarg paths log and return **0xE000_02C2** (`w19 -= 0x17` trick)
5. call AB helper; pass its return through (0 = success); non-zero → IOLog + return it

### 4.1 submitFrame ✅ — body 0xd708–0xdc64, tramp 0xcd7c (`tmp/dis_uc_submitFrame.txt`)

`u32 UC::submitFrame(s_inputDataStruct* in, u64* out)`
- Validates: in, in->ptr, in->size, out, `in->slot_idx < [UC+0x18F0]`.
- Creates **input** descriptor: `IOMemoryDescriptor::withAddressRange(ptr, size, dir=2, task=[UC+0xE0])`
  (PLT GOT[24]); wire-check `[vt+0x218](desc_in, type=2)` — non-zero → log + release desc.
- Loads **output** descriptor from slot: `entry = UC + slot_idx*0x18; out_desc = [entry+0xF0]`;
  if NULL → error log (no bound buffer). Validity check `[out_desc_vt+0x1E8](out_desc)` must == 1.
- Calls **`AB::submitFrame(sContext*, IOMemoryDescriptor* in, IOMemoryDescriptor* out, u64*, u32, u32, u32, sDescriptorLengthOffsetInfo)`** @0xa9ac —
  args: x0=AB(this), x1=&UC+0x18F8 (sContext*), w7=in->flag(+0x20), last arg built from slot entry.
- Return handling of AB result (w22):
  - `== 0` → success: `[UC+0x1958] = out[0]`; call `[out_desc_vt+0x1F8](out_desc, 1)`; return **0**
  - `== 0xE000_02D8` ("no space") → special path @0xdbfc (log; no desc teardown)
  - other non-zero → error log + `[in_desc_vt+0x220](desc_in, 0)` teardown + release
- kernel_debug trace points: `kernel_debug(0x2B61FA4, class=3/4/5, ...)` at 0xd858 / 0xdaac / 0xdb38.

### 4.2 queryCompletion ✅ — body 0xdcc4–0xdd9c (`tmp/dis_uc_small.txt`)

`u32 UC::queryCompletion(u64 scalar, u64* out)`
- Validates scalar-block ptr and out ptr (else return 0xE000_02D9).
- Calls **`AB::queryCompletion(sContext*, u64, u64*)`** @0xb88c — args: x0=AB(this),
  x1=&UC+0x18F8 (sContext*), x2=scalar (frame id?), x3=out.
- Returns AB result directly: **0 = success** (completion info in *out); non-zero → IOLog(fmt@0x2C00 — note: string page 0x2xxx) + return it.

### 4.3 bindFrame ✅ — body 0xdd9c–0xe2a4 (`tmp/dis_uc_bindFrame.txt`)

`u32 UC::bindFrame(s_bindDataStruct* in, u64* out)` — out[0]=slot idx, out[1]=0
- Slot selection (dd9c..de8c): uses slot count `[UC+0x18F0]`, cap check `count <= 0xFF`;
  exact auto-assign vs explicit-index logic 🟡 TODO.
- **Existing-slot path** (slot.desc != NULL):
  - `[desc_vt+0x200](desc) == 1` → frame in use by firmware → error log (existing-slot err@0x33E9), return err
  - else **replace**: `[old_vt+0x220](old, 0)` teardown + release; zero slot f2/size fields; fall through to new-bind.
- **New-bind path** @0xe058:
  - `aligned = align_up(ptr & ~mask + size, mask)` (overflow-checked); local `[sp-0x48]` = aligned_size
  - `buf_offset = ptr - (ptr & ~mask)` → x26
  - `desc = IOMemoryDescriptor::withAddressRange(aligned_ptr, aligned_size, dir=1, task=[UC+0xE0])`; NULL → freeSpace rollback + log(desc-fail@0x344F)
  - `[vt+0x218](desc, type=1)` wire-check; non-zero → **rollback**: `AB::freeSpace(&aligned_size)` + `UC_counter -= aligned_size` (writes @0xe154/@0xe1f8) + log(wire-fail@0x3429) + release desc
  - success: store slot entry `{desc, buf_offset, size}` at `UC + idx*0x18 + {0xF0,0xF8,0x100}`; if `idx == count` → `count++` ([UC+0x18F0]); out[0]=idx, out[1]=0; return 0.

### 4.4 cardStatus ✅ — body 0xe2a4–0xe36c (`tmp/dis_uc_small.txt`)

`u32 UC::cardStatus(u64* out)`
- badarg (out==NULL) → **0xE000_02C2**.
- Calls **`AB::cardStatus(u64*)`** @0xbd90 — args: x0=AB(this), x1=out. No sContext arg.
- Returns AB result directly; non-zero → IOLog(fmt@0x347B, name@0x34A9).
- *out semantics (decoded §6): trivial read of **AB+0xC0** — the engine-maintained status word.

### 4.5 checkForSpace ✅ — body 0xe36c–0xe4c0 (`tmp/dis_uc_checkForSpace.txt`)

`u32 UC::checkForSpace(s_bufDataStruct* in)`
- badarg (in/ptr/size NULL) → log(fmt@0x34B4, name@0x34F2) + **0xE000_02C2**.
- `aligned_size = align_up(ptr & ~mask + size, mask)` overflow-checked.
- Calls **`AB::checkForSpace(u64* aligned_size)`** @0xbfec (reserves; 0xE000_02EC if cap exceeded).
- On success: **`UC_counter += aligned_size`** (write @0xe440).
- Return value quirk: returns 0xE000_02C2 even on the success path (outScalars=0 → discarded by trampoline; observable effect is purely the reservation side effects).

### 4.6 unbindAll ✅ — body 0xe4c0–0xe6a4 (`tmp/dis_uc_unbindAll.txt`)

`u32 UC::unbindAll()`
- AB==NULL or inactive → **0xE000_02D9**.
- First: `AB::cleanUp(ctx, &UC+0x18F8)` @0xb0ec (card-side state reset).
- Loop i = 0..191 (`x22` from -0x1800 stepping +0x18 until 0): `desc = [slot_i.desc]`; skip NULL.
  - `[desc_vt+0x200](desc) == 1` → in use by firmware: IOLog(fmt@0x3500, name@0x33E9), return **0xE000_02C2** (via `w19 -= 0x17`)
  - else: `AB::unbindFrame(ctx, desc)` @0xbf6c; `len = [desc_vt+0x1F0](desc)`; **`UC_counter -= len`** (@0xe5a4); `[vt+0x220](desc, 0)` teardown; release.
- After loop: `[UC+0x18F0] = 0`; zero UC+0xF8/UC+0x100 (slot 0 f2/size); return **0**.

### 4.7 getPercent ✅ — body 0xe6a4–0xe76c (`tmp/dis_uc_getPercent.txt`)

`u32 UC::getPercent(u64* out)`
- badarg → **0xE000_02C2**.
- Calls **`AB::getPercent(u64*)`** @0xbda0 — args: x0=AB(this), x1=out.
- Returns AB result directly; non-zero → IOLog(fmt@0x350A, name@0x353D).
- *out semantics (decoded §11): **idle-time fraction** since last call — NOT decode progress.

## 5. UC object layout (offsets from the `this` pointer) ✅/🟡

| offset | field | notes |
|---|---|---|
| +0xD8   | AB provider (`AppleAfterburner*`) | NULL until start(); all methods bail if NULL |
| +0xE0   | task port | passed to withAddressRange; saved by initWithTask |
| +0xF0 .. +0xCF0 | **slot table**: 192 × 24B `{IOMemoryDescriptor* desc@+0x0, u64 buf_offset@+0x8, u64 size@+0x10}` | bound output buffers; zeroed by initWithTask (movi loop) |
| +0x18F0 | slot count (high-water mark) | 32-bit; bindFrame increments when idx==count; unbindAll/initWithTask reset to 0 |
| +0x18F8 | `sContext` struct (card-side context) | passed **by address** to every AB helper that takes sContext* |
| +0x1900 | ? zeroed by unbindAll after loop | 🟡 semantics TODO |
| +0x1958 | last successful submitFrame `out[0]` (frame id/seq?) | written @0xdb5c |
| +0x1960 | IOLock* | allocated in initWithTask; locked by every trampoline |
| +0x1968 | **reserved-space counter** (bytes) | lifecycle below |

### UC+0x1968 write sites (all 6, verified by full __text scan) ✅
| site | function | op |
|---|---|---|
| 0xd154 | initWithTask | `= 0` |
| 0xe440 | checkForSpace | `+= aligned_size` (after successful AB reservation) |
| 0xe154 / 0xe1f8 | bindFrame | `- aligned_size` (rollback on wire-check/desc-creation failure, after AB::freeSpace) |
| 0xe5a4 | unbindAll | `- desc_len` per released slot |
| 0xd2f0 | stop() | `- desc_len` per released slot (client-disconnect teardown loop) |

## 6. AB provider (`AppleAfterburner`) helpers

**Space pool** — all under `IOLock* AB+0x12C0`; **used = AB+0x12E8**, **cap = AB+0x12F0**.
Note: demangled signatures say `Py` (u64) but checkForSpace/freeSpace actually take a
**pointer to u64 size** (`ldr x9,[arg]` deref verified).

| fn | addr | signature | behavior | status |
|---|---|---|---|---|
| bindFrame(desc) | 0xbec | `(IOMemoryDescriptor*)` | `len=[desc_vt+0x1F0](desc)`; **commits** `used += len` FIRST, then if `used > cap` → return **0xE000_02EC** (pool left over-cap — caller must roll back via freeSpace); else 0 | ✅ (`tmp/dis_ab_space_fns.txt`) |
| unbindFrame(desc) | 0xbf6c | `(IOMemoryDescriptor*)` | `len=[desc_vt+0x1F0](desc)`; `used = max(0, used - len)` (saturating); return 0. Called per-slot by UC::unbindAll/stop | ✅ (`tmp/dis_ab_space_fns.txt`) |
| checkForSpace | 0xbfec | `(u64* size)` | if `used > cap` → **0xE000_02EC**; else `used += *size`; return 0 | ✅ (`tmp/dis_ab_checkForSpace.txt`) |
| freeSpace | 0xc058 | `(u64* req)` | `used = max(0, used - *req)` (saturating); does NOT write back *out; return 0. Rollback path in UC::bindFrame + UC::stop | ✅ (`tmp/dis_ab_freeSpace.txt`) |
| cleanUp | 0xb0ec–0xb634 | `(sContext*)` | ✅ **§15.1** — two-pass queue finalization; guard-flag skip path; engine-wait(timeout=0) + PAC'd AB::reset callback on second call | ✅ (`tmp/dis_ab_cleanUp.txt`) |
| **reset** | **0xaea8–0xb0ec** | `()` | **per-frame teardown**: clear both doorbells, wait for c2h/h2c status to drain, free 2 desc queues, fw handshake, reset all completion tracking. See §10.3 — CRITICAL for Linux driver | ✅ (`tmp/dis_ab_reset.txt`) |
| queryCompletion | 0xb88c–0xbd90 | `(sContext*, u64 frameID, u64* out)` | **NEVER writes *out** (x3 clobbered @0xb908; no store to it anywhere). Waits on engine via PAC'd stub, then drains 4 DMA completion queues. See §10 | ✅ (`tmp/dis_ab_queryCompletion.txt`) |
| submitDataGated | 0x985c–0x9be0 | `(sDmaConfig*)` | ✅ **§15.2** — the real submission core: wake-wait → guard-flag check → flow control (engine vt+0x210) → completion word → queue push/coalesce → doorbell ring on both channels. Engine callback #22; no direct `bl` callers | ✅ (`tmp/dis_ab_completion_fn.txt`) |
| cardStatus | 0xbd90–0xbda0 | `(u64* out)` | **trivial**: `*out = [AB+0xC0]` (u32 status word); return 0. AB+0xC0 is the engine-maintained status field (stub passes &AB+0xC0 to engine poll) | ✅ (`tmp/dis_ab_cardStatus_getPercent.txt`) |
| getPercent | 0xbda0–0xbe64 | `(u64* out)` | **idle-time fraction** since last call: `pct = acc_E8·100/(acc_E0+acc_E8)`, then resets both accumulators. See §12 | ✅ (`tmp/dis_ab_cardStatus_getPercent.txt`) |
| updateActiveTime | 0xbe64–0xbec | `()` | idle→active transition: `[AB+0x12E0]=0`; flush elapsed interval to **acc_E8**; return 0. Called from handleInterrupt @0x64c8 | ✅ (`tmp/dis_ab_space_fns.txt`) |
| updateIdleTime | 0x9be0–0x9c5c | `()` | active→idle transition: `[AB+0x12E0]=1`; flush elapsed interval to **acc_E0**; return 0. Called from submitDataGated @0x9b18 | ✅ (`tmp/dis_ab_updateIdleTime.txt`) |
| submitFrame | 0xa9ac | `(sContext*, IOMemoryDescriptor* in, IOMemoryDescriptor* out, u64*, u32, u32, u32, sDescriptorLengthOffsetInfo)` | two-stage (input/output) frame submission; double-checked busy flag; `ctx->+0x160` = thermal distress code → frame drops when non-zero; exit paths: 0xacfc busy-under-lock, 0xad04 busy-unlocked, 0xad28 cfg==0→submitData branch | 🟡 head+mid decoded (`tmp/dis_ab_submitFrame_head.txt`, `_mid.txt`) |
| submitData | 0xa67c | (vtable wrapper on self->+0xa0) | doorbell/staging commit: send=clear +4 / stage BAR1+0x4080..C / ring 0xFFE7F; batch=clear +0x1004 / stage +0x5080..C / ring 0x80FFE7F | ✅ (byte-verified vs Linux driver, FINDINGS §25) |
| getSegments | 0xa28c | — | segment list for frame path: seg list self->+0x11d8 ({PA,len}×N), N=self->+0x11e0>>4 | ✅ (`tmp/dis_ab_getSegments.txt`) |

## 7. IOMemoryDescriptor vtable anchors (offsets from desc's vptr)

| vt offset | call | semantics |
|---|---|---|
| +0x28   | `(desc)`            | release (refcount--) — Itanium slot 3 |
| +0x1E8  | `(desc)` → int      | validity/ready check for **output** buffers; submitFrame requires == 1 |
| +0x1F0  | `(desc)` → u64      | getLength (used by unbindAll/stop for counter decrement) |
| +0x1F8  | `(desc, 1)`         | called on submitFrame success path — semantics 🟡 TODO |
| +0x200  | `(desc)` → int      | "frame in use by firmware?"; ==1 blocks rebind (bindFrame) and unbindAll aborts with 0xE000_02C2 |
| +0x218  | `(desc, type)` → err | post-creation wire/validate; type matches direction arg (1=bind/out-buffer, 2=submit/input); 0=success |
| +0x220  | `(desc, 0)`         | teardown/detach before release |

`withAddressRange(ptr, size, dir, task)` via PLT GOT[24]; direction values observed:
bindFrame passes **1**, submitFrame passes **2** (xnu `IODirection` enum — verify mapping).

## 8. Error code table ✅

| code | meaning | raised by |
|---|---|---|
| 0xE000_02C2 | base error: badarg / frame-in-use-by-firmware / generic failure | all methods (badarg via `w19 -= 0x17` from 0x2D9); unbindAll in-use abort; checkForSpace success-path quirk |
| 0xE000_02D8 | no space (submitFrame special path @0xdbfc) | AB::submitFrame return |
| 0xE000_02D9 | no provider / service inactive | default in every UC method wrapper |
| 0xE000_02EC | card space cap exceeded (`used > cap`) | AB::checkForSpace |

## 9. Log string inventory (page-relative: real = page_base + offset) ✅

Strings live on two pages: `adrp #0x3000` and `adrp #0x2000`.

| method | fmt | name/other |
|---|---|---|
| submitFrame (UC) | 0x31C9, 0x3206, 0x324F, 0x328E, 0x32D9, 0x3325 | name@0x2695 |
| queryCompletion | name@**0x2C00** (page 0x2xxx!) — leak fmts at 0x2BAB/0x2C10, see §9 note below | — |
| bindFrame | badarg@0x339A · existing-slot err@0x33E9 · wire-fail@0x3429 · desc-fail@0x344F | name@0x3390 |
| cardStatus | 0x347B | name@0x34A9 |
| checkForSpace | 0x34B4 | name@0x34F2 |
| unbindAll | in-use fmt@0x3500 | name@0x33E9 (shared with bindFrame) |
| getPercent | 0x350A | name@0x353D |
| AB::submitFrame | ab.sf fmt1@0x2638, fmt2@0x2695 | — |
| queryCompletion (AB) | dmaQ-leak fmt@**0x2BAB** · descDmaQ-leak fmt@**0x2C10** | fn-name 'queryCompletion'@**0x2C00** (used as the `%s::%s` middle arg — earlier note had these swapped) |
| engine-query stub (`queryCompletionGated`) | timeout fmt@**0x2B27**: `"AppleAfterburner Error: %s[%p]::%s: Query timed out (resetting), frameNum %llu, completionStamp %lld, _numFrames %u, status 0x%08x\n"` | fn-name 'queryCompletionGated'@**0x2B12** |
| reset | c2hStatus fmt@**0x2958** · h2cStatus fmt@**0x2994** · "Decoder status: 0x%08x"@**0x29CA** | — |
| submitDataGated | Wake-fail fmt@**0x2326**: `"AppleAfterburner Error: %s[%p]::%s: Wake: 0x%08x"` · hang fmt@**0x2367**: `"...Possible hang, frameNum %llu, completionStamp %lld\n"` | fn-name 'submitDataGated'@**0x2357** |
| cleanUp | decoder-error fmt@**0x2A05**: `"AppleAfterburner Error: %s[%p]::%s: Decoder error (resetting) _frameNum %llu, completionStamp %lld, _pendingData %u, status 0x%08x\n"` | fn-name 'cleanUp'@**0x2A89** |

kernel_debug trace ids: **0x2B61F9C / 0x2B61FA4** (handleInterrupt completion loop) · **0x2B61FB0** (submitDataGated ch1/batch doorbell ring — args: self, [cfg+0x3C], 1/*ch*/, [self+0xC0], 0).

## 10. AB::queryCompletion deep dive ✅ (body 0xb88c–0xbd90, `tmp/dis_ab_queryCompletion.txt`)

`u32 AB::queryCompletion(sContext* ctx, u64 frameID, u64* out)` — **never writes *out**
(x3 is clobbered @0xb908 and no store to it exists in the body; userland gets completion
state via side effects / other channels).

### 10.1 Engine wait (the actual "wait for frame" mechanism)

```c
engine = [AB+0x90];                       // engine object (PAC-verified vptr, tag 0xcda1)
ret = [engine_vt+0x1E8](engine, pacia(stub@0xB634, key=0xBD41), frameID, &u32_local(=2), 0, 0);
if (ret != 0) return ret;                 // hard failure → propagate to UC wrapper
```

- `&u32_local` initialized to **2** — timeout/iteration bound passed by pointer.
- The stub is a **deadline-bounded poll loop** (`tmp/dis_ab_engine_query_stub.txt`,
  fn-name string 'queryCompletionGated'@0x2B12):

```c
// stub(AB* self, u64* prog, u32* fid) — always returns 0 except engine error:
if ([self+0x12C8] && *prog >= [self+0x12D0]) {          // "done" fast path (flag set by IRQ/first timeout)
    if (([self+0xC8] & ~[self+0x12D0]) != [self+0x12D8]) goto fail;   // status-word verify
    IOLog(fmt@0x2B12-name, ...); AB::reset(self);       // @0xaea8 — engine reset AFTER each completed frame!
    return 0;
}
if (*prog <= [self+0x1270]) return 0;                   // progress below completionStamp → "keep waiting"
for (;;) {
    clock_interval_to_deadline(0x7D0, 0x4240, &deadline);   // interval=2000, max=17008 (mach ticks)
    r = [engine_vt+0x210](engine, &self->status@AB+0xC0, &deadline, *fid);  // poll w/ deadline
    if (r == 1) {                                        // timeout tick:
        first_time ? ([self+0x12C8]=1; [self+0x12D8]=0; x=[self+0x12D0]=*prog) : (x=[self+0x12D0]);
        if (([self+0xC8] & ~x) != [self+0x12D8]) goto fail;
        IOLog("Query timed out (resetting), frameNum %llu, completionStamp %lld, _numFrames %u, status 0x%08x");
        AB::reset(self); return 0;
    }
    if (r != 0) return 0xE000_02BC;                      // engine error
    if (*prog > [self+0x1270]) goto done_check;          // crossed stamp → re-run fast path
    return 0;                                            // still waiting
fail:
    [self+0x12D8] += 1;                                  // _numFrames failure counter
    IOLog(...); AB::reset(self); return 0;
}
```

**AB completion-tracking fields:** +0xC0 = status word (engine-maintained; also what
cardStatus() returns) · +0xC8 = second status/counter word · +0x1270 = **completionStamp**
(expected progress value) · +0x12C8 = done-flag byte · +0x12D0 = stamp/mask for verify ·
+0x12D8 = **_numFrames** (u32; failure counter, cleared by IRQ handler & first timeout) ·
+0x12DC = status u32 written by handleInterrupt.

### 10.2 Queue drain (after engine wait returns 0)

Two queue types × two instances each (fixed i ∈ {0,1} — the "loop" is exactly 2 passes):

| queue | head location | lock location |
|---|---|---|
| dmaQ[0]      | ctx+0x00 (self-ref sentinel when empty) | [ctx+0x40] |
| dmaQ[1]      | ctx+0x10                                  | [ctx+0x48] |
| descDmaQ[0]  | ctx+0x20                                  | [ctx+0x50] |
| descDmaQ[1]  | ctx+0x30                                  | [ctx+0x58] |

Per drained node (node = 24B `{next@0, pad@8, obj@0x10}`, freed via `IOFree(node, 24)`):
```c
obj = node->obj;                       // frame object, PAC-verified vptr
comp = [obj_vt+0x158](obj);            // vt slot 67: get completion object (NULL → leak warning)
if (!comp) { IOLog("No entry ... in dmaQ/descDmaQ - potential leak"); /* node NOT freed */ }
else {
    [obj_vt+0x150](obj, 1);            // vt slot 66: ack(obj, 1)
    notifier = (dmaQ ? [AB+0xA0] : [AB+0xA8]);   // two distinct notifier objects!
    [notifier_vt+0x150](notifier, obj);          // notify(notifier, obj)
    IOFree(node, 24);
    [comp_vt+0x220](comp, 0);          // vt slot 136: teardown(comp, 0)
    [comp_vt+0x28](comp);              // release comp
}
```

### 10.3 AB::reset() — per-frame teardown ✅ (body 0xaea8–0xb0ec, `tmp/dis_ab_reset.txt`)

Called by the query stub after **every** completed frame (and on timeout/failure). This is
the exact between-frame sequence the Linux driver must replicate:

```c
void AB::reset() {
    // 1. volatile read of decoder status [AB+0x158]+0x14 (no-op store)
    BAR1 = [self+0x130];                       // BAR1 base (confirms Phase-3q: ctx->+0x130)
    [BAR1+4]     = 0;                          // clear h2c doorbell (ring 0xFFE7F)
    [BAR1+0x1004]= 0;                          // clear c2h/batch doorbell (ring 0x80FFE7F)
    if ([self+0x158]) [[self+0x158]+4] = 0;

    // 2. wait for both channel status words to drain (bit0 = valid):
    if ([BAR1+0x1044] & 1) { IOSleep(1000ms); if ([BAR1+0x1044]) IOLog("c2hStatus 0x%08x"); }   // fmt@0x2958
    if ([BAR1+0x44]     & 1) { IOSleep(1000ms); if ([BAR1+0x44])     IOLog("h2cStatus 0x%08x"); }   // fmt@0x2994

    // 3. free the two descriptor queues on AB itself (72-byte nodes, self-ref sentinel):
    for (i = 0; i < 2; i++) {
        head_loc = &self[i*0x10 + 0x11F0];     // queue heads at AB+0x11F0 and AB+0x1200
        while ((node = *head_loc) != head_loc) { next = node[0]; *head_loc = next; IOFree(node, 72); }
    }

    // 4. firmware handshake: signal reset, wait for decoder ready (100ms sleep loop):
    do { if ([self+0x158]) [[self+0x158]+4] = 1; else IOSleep(100ms); }
    while (!([self+0x158] && ([[self+0x158]+8] & 1)));
    IOLog("Decoder status: 0x%08x", [obj+8]);  // fmt@0x29CA

    // 5. reset all completion tracking:
    [self+0x1308] = [self+0xC8];               // save status word as IRQ-stamp base
    [self+0x1270] = [self+0xC8] - 1;           // completionStamp = status-1 (next expected)
    [self+0xC0]   = 0;                         // clear cardStatus word
    [self+0xD8]   = 0;                         // clear IRQ-stamp high component
    [self+0x12C8] = 0;                         // clear done-flag byte (via x20=self+0x11E4, strb +0xE4)
    [self+0x12D0] = -1;                        // verify mask = all-ones
    [self+0x12D8] = 0;                         // _numFrames = 0 (u64 zero store)
}
```

**Naming confirmed by log strings:** BAR1+0x1044 = **c2hStatus** (card→host completion word),
BAR1+0x44 = **h2cStatus** (host→card data/completion word). AB+0x158 = **decoder control
object**: +4 = reset-signal, +8 = decoder status (bit0 = ready/ack), +0x14 = volatile status.

## 11. Time accounting (getPercent / updateActiveTime / updateIdleTime) ✅

- **AB+0x12E0** = idle-flag byte: set by `updateIdleTime` (active→idle), cleared by
  `updateActiveTime` (idle→active).
- **AB+0xE0** = acc_E0 (u64, active-time accumulator) · **AB+0xE8** = acc_E8 (idle-time
  accumulator) · **AB+0xF8** = last-sample timeval · **AB+0x108** = scratch timeval
  (microtime target).
- `getPercent`: elapsed = now − [F8]; flag set → acc_E8 += elapsed, else acc_E0 += elapsed;
  `*out = acc_E8·100/(acc_E0+acc_E8)` (**fraction of time IDLE**); both accumulators reset
  to 0 afterwards. So userland sees a *utilization complement*: low % = card busy decoding.
- Callers (full __text scan): `updateActiveTime` ← handleInterrupt @0x64c8;
  `updateIdleTime` ← submitDataGated @0x9b18.

## 12. AB::handleInterrupt ✅ (body 0x5aec–~0x69f8, `tmp/dis_ab_handleInterrupt.txt`)

IRQ entry point (`(OSObject*, IOInterruptEventSource*, int)`). **Full body decoded 2026-11-05.**
Structure: prologue → bitfile section → thermal section → completion section (gated by AB+0x11B4)
→ deferred re-submission chain → exit. All PLT targets resolved via `stub_map.json` + `imports_map.json`.

### 12.0 Prologue @5aec
```
x19 = self; x24 = self + 0x11B4        ; completion-pending flag address (base reg for field group)
w22 = 0x2B61FAC                          ; kernel_debug trace ID base (all traces in this fn use w22+N)
x8  = [self+0x148]                       ; BAR0+0x2000 region (bitfile control block)
w20 = [BAR0+0x2014]                      ; bitfile interrupt word
if (!(w20 & 1)) → thermal section @6140  ; bit0 clear → skip bitfile handling entirely
```

### 12.1 Bitfile section (bit0 of BAR0+0x2014 set)
- IOLog("handleInterrupt" fmt@0x2cb3, name, w20); kernel_debug(0x2B61FAC, self, 1, w20, 0, 0) [trace #1]
- w20 = **[BAR0+0x2010]** — bitfile status LIVE → IOLog("bitfile status live: 0x%08x" fmt@0x2cc3)
- w21 = **[BAR0+0x2020]** — rising latched event word → IOLog("bitfile rising latched" fmt@0x2d03)
- if ((w21 & 7) == 0) → falling check @60e0 (no rising events)
- else: **[BAR0+0x2020] = w21 & 7** (ACK rising latch, masked to bits 0–2)
  - **bit2 (PR programming error)**: [BAR0+0x2004] = 0 (engine disable); IOLog("PR programming error!" fmt@0x2d46)
  - **bit1 (partial reconfig)**: [BAR0+0x2004] = 0; IOLog("partial FPGA reconfig event" fmt@0x1d81) → flash/bitfile walk @5d14

### 12.2 Flash/bitfile directory walk @5d14 (only on PR bit1)
```
idx=0; [self+0x158]=NULL; w26=1
loop:
   word = [[self+0x128] + idx*4]          ; u32 table at AB+0x128 (directory base ptr)
   IOLog("Section header: 0x%08x" fmt@0x1aca, name, word)
   type = word & 0xFF
   if (type == 1):                        ; decoder section!
       [self+0x11D0] = 1                  ; decoder-found flag
       [self+0x158] = table_base + idx    ; decoder section pointer
   next_idx = ((word >> 14) & 0x3FFFC) + idx
   if (type != 0) → loop with next_idx    ; type==0 = terminator
```
Matches Phase 3k live walk exactly (type=W&0xFF, next=O+((W>>14)&0x3FFFC)).

**Decoder disable/enable cycle** (after walk):
- IOLog("Decoder section header" fmt@0x1b05) with [decoder_section] word 0
- **[decoder_section+4] = 0** (DECODER DISABLE — BAR0[hdr+4])
- IOLog("Decoder Control Register ... (disabling)" fmt@0x1b48); verify-read + log
- **IOSleep(1)** (PLT stub 0xe89c)
- **[decoder_section+4] = 1** (DECODER RE-ENABLE)
- Re-enable log: fmt@0x11DF if decoder found, else fmt@0x1B99. Note: kext references a mid-string pointer here (cosmetic logging bug in Apple's code).

### 12.3 PR DMA reset @5ee0 (only if rising bits 1|2 were set)
```
if (!(w21 & 6)) → skip to flag clear @60dc
   [PR event:]
   x21 = [self+0x11C8]                    ; descDmaQ queue OBJECT ptr!
   sub = [x21_vt+0x158](...)              ; get sub-object (DMA state)
   ... multiple vtable calls to tear down DMA state ...
   x20 = [self+0x11C0]                    ; dmaQ queue OBJECT ptr
   ... more teardown vcalls ...
60dc: [self+0x11B4] = 0                   ; clear completion-pending flag
```
**Correction:** AB+0x11C0/AB+0x11C8 are **OBJECT POINTERS** to the dmaQ/descDmaQ queue objects (not raw notify registers). The PR path tears down DMA state via vtable calls and nulls these pointers.

### 12.4 Falling latched check @60e0
- w20 = **[BAR0+0x2024]** — **bitfile falling-latched event word** (correction: NOT a completion status register; real completion is at BAR1+0x2044)
- IOLog("bitfile falling latched" fmt@0x2dba)
- if ((w20 & 7)): **[BAR0+0x2024] = w20 & 7** (ACK, masked to bits 0–2)

### 12.5 Thermal section @6140 (BAR0+0x1000 region via [self+0x140])
```
w20 = [BAR0+0x1014]                       ; thermal interrupt word
if (!(w20 & 1)) → completion section @639c
   [thermal IRQ:]
   IOLog("thermal interrupt" fmt@0x2dfe, name, w20)
   w20 = [BAR0+0x1010]                    ; thermal status LIVE
   IOLog("thermal status live" fmt@0x26a1, ...)
   w20 = [BAR0+0x1020]                    ; rising latched (raw)
   IOLog("thermal rising latched" fmt@0x2e3c, ...)
   w21 = w20 & 0x7D                       ; mask to valid bits
   if (w21):
       if ([self+0x160] == 0) → IOLog("entering thermal distress state" fmt@0x2e7f)
       [self+0x160] |= w21                ; accumulate rising bits into distress mask!
       [BAR0+0x1020] = w21                ; ACK (masked value!)
   w21 = [BAR0+0x1024]                    ; falling latched (raw)
   IOLog("thermal falling latched" fmt@0xec4, ...)
   w26 = w21 & 0x7D
   if (w26):
       [self+0x160] &= ~w26               ; clear falling bits from distress mask!
       if ([self+0x160] == 0) → IOLog("exiting thermal distress state" fmt@0xf08)
       [BAR0+0x1024] = w26                ; ACK (masked value!)
kernel_debug(0x2B61FAC, self, 2, live_status, falling_raw, 0)   ; trace #2
```
**AB+0x160 = thermal distress bitmask** (NOT a counter): rising bits OR'd in, falling bits AND-NOT'd out. Non-zero → submitFrame drops frames (§13). First-event log only on 0→nonzero transition; recovery log only on nonzero→0 transition.

### 12.6 Completion section @639c (gated by AB+0x11B4)
```
w8 = [self+0x11B4]                        ; completion-pending flag (byte)
if (!flag) → decoder frame-done check @63f0
   [flag set:]
63a4: x8=[self+0x130]; w25=[BAR1+0x2044]; [BAR1+0x2018]=w25   ; read status + ACK #1!
      x20 = w25 >> 1                       ; dispatch value (bit1 = channel selector)
      kernel_debug(0x2B61FAC, self, 4, x20, 0, 0)             ; trace #4
      if (x20 == 1) → single batch path @6554
      if (x20 != 0) → shared entry @6668 with w26=0 (no data word)
      [x20 == 0 — send channel:]
        w26 = [BAR1+0x44]; wzr = [BAR1+0x48]    ; SEND data words!
        → shared entry @6668

; --- Decoder frame-done check (flag clear) @63f0 ---
ptr = [self+0x158]                        ; decoder section pointer
if (!ptr || !([ptr+0x14] & 0x1F)) → status read @63a4   ; no error → normal path
   [frame-done bits set — DECODER ERROR:]
   IOLog("Error from decoder" fmt@0x2f4c, name, w20)
   kernel_debug(0x2B61FAC, self, 3, w20, 0, 0)             ; trace #3
   if (!(w20 & 1)) → status read @63a4                     ; soft error → continue normally
   [HARD ERROR (bit0 set):]
   IOLog("HARD error from decoder" fmt@0x2f8d, name, w20)
   updateActiveTime()                                   ; ← idle→active transition!
   **[self+0x12C8] = 1**                                ; SET guard flag (enables overrun check)!
   [self+0x12DC] = [decoder+0x10]                        ; save offending frame index
   cursor = [self+0x1308] + ([decoder+0x10]>>16) + ([self+0xD8]<<16)
   [self+0x12D0] = cursor                                ; set overrun limit!
   [self+0x12D8] = 0                                     ; clear failure counter
   IOLog("Offending frameNum" fmt@0x2fd3, name, cursor)
   → EXIT @6958 (epilogue)
```

**BAR1+0x2044 format:** bit0 = valid indicator (per Phase 3i live decode), bit1 = channel selector (0=send/h2c completion with data @BAR1+0x44/0x48, 1=batch/c2h single completion). Dispatch uses status>>1: 0→send path, 1→single batch path, >1→shared entry without data word.

**Guard flag AB+0x12C8 lifecycle (fully resolved):**
- SET by queryCompletionGated@b7b4 (first timeout)
- **SET on hard decoder error @64d0** (this function — enables overrun check for subsequent completions)
- CLEARED by reset()@b0c0

### 12.7 Single batch completion path @6554 (x20==1, ch==1)
```
ptr = [self+0x158]                        ; decoder section pointer
w20 = ptr ? [ptr+0x10] : 0                ; frame index from decoder!
kernel_debug(0x2B61FB4, self, w2=[AB+0x12E4], x3=?, w4=[AB+0xC0], ...)   ; trace #5 (w22+8)
[AB+0x12E4]--                             ; expected count--
[AB+0xC0]--                               ; _pendingData--
notifier = [self+0x90]; PAC-verify vptr; [vt+0x208](notifier, self, 0)   ; NOTIFY (arg1=self!)
if ([AB+0x12E4] != 0) → drain loop @67a4  ; more expected → continue draining
```

### 12.8 Shared queue entry + drain loop @6668
```
[BAR1+0x2014] = w25                       ; ACK #2 (always, before queue processing)
sentinel_addr = self + ch*16 + 0x11F0     ; per-channel queue sentinel (ch0: AB+0x11F0, ch1: AB+0x1200)
head = [sentinel_addr]                    ; head node ptr (== sentinel if empty)

if (head == sentinel):                    ; EMPTY QUEUE
    if (w26 & 1) → EXIT @6958             ; data bit set but no queue → exit
    if (ch==1 && [AB+0x12E4]==0) → flag clear @685c
    else → drain loop entry @67a4 with x21=&AB+0xC0, w23=0xFFFF

[non-empty:]
if (w26 & 1) → EXIT @6958                 ; data bit set → exit without processing!
if (ch==1 && [AB+0x12E4]==0):             ; flag-set path @6760
    **[self + ch + 0x11E4] = 1**          ; SET channel active flag!
    if (head == sentinel) → node processing @68a4 with x21=NULL (invariant violation!)
    else: pop node; → node processing @68a8

[otherwise:] drain loop entry @669c with x21=&AB+0xC0, w23=0xFFFF

; --- Drain loop @66a4–6758 ---
loop:
   ptr = [self+0x158]                     ; decoder section pointer
   idx = ptr ? ([ptr+0x10] >> 16) : (w9 stale — in practice non-NULL here)
   cursor = [self+0x1308] + idx + ([self+0xD8]<<16); [self+0x1270] = cursor
   if (idx == 0xFFFF && cursor >= 1): [self+0xD8]++
   kernel_debug(0x2B61FB4, self, w2=old_count, x3=?, w4=[AB+0xC0], 0)   ; trace #5
   [self+0x12E4]--                        ; expected count--
   [self+0xC0]--                          ; _pendingData--
   notifier = [self+0x90]; PAC; [vt+0x208](notifier, x21, 0)            ; NOTIFY (arg1=x21!)
   if ([self+0x12E4] != 0) → loop         ; more expected → drain again

[drain complete — count==0:]
head = [sentinel_addr]                    ; reload head
**[self + ch + 0x11E4] = 1**              ; SET channel active flag! (new work coming!)
if (head == sentinel) → node processing @68a4 with x21=NULL (invariant!)
else: pop node; → node processing @68a8   ; RE-SUBMIT next deferred node!
```

**Notify semantics:** `[AB+0x90]->[vt+0x208](notifier, ctx, 0)` where ctx = self (single completion path) or &AB+0xC0 (drain loop). The notifier object's class is unknown — open item.

### 12.9 Node processing / deferred re-submission @68a4–69c8
```
; --- SEND channel node (type==0) ---
w9 = node[+0x10]; w8 = node[+0x14]        ; desc PA lo/hi (staging words!)
w10 = node[+0x1C]                         ; type
if (type == 1) → batch re-submission @6978
if (type != 0) → free only @69c8
   [type == 0 — SEND:]
   x10 = BAR1 base
   [BAR1+4] = 0                           ; clear send doorbell
   [BAR1+0x4080] = w9                     ; stage desc PA lo
   [BAR1+0x4084] = w8                     ; stage desc PA hi
   [BAR1+0x4088] = node[+0x18]            ; stage local_val
   [BAR1+4] = 0xFFE7F                     ; RING send doorbell!
   → free @69c8

; --- BATCH re-submission (type==1) @6978 ---
[BAR1+0x1004] = 0                         ; clear batch doorbell
[BAR1+0x5080] = node[+0x10]               ; stage desc PA lo
[BAR1+0x5084] = node[+0x14]               ; stage desc PA hi
[BAR1+0x5088] = node[+0x18]               ; stage local_val
[BAR1+0x1004] = 0x80FFE7F                 ; RING batch doorbell!
w2 = node[+0x3C]                          ; expected completion count from the node!
[self+0x12E4] = w2                        ; SET expected count (batch!)
kernel_debug(0x2B61FB0, self, 1, [AB+0xC0], ...)   ; trace #6 (w22+4)

; --- Free + exit @69c8 ---
mov x0, x21; mov w1, #0x48                ; kfree(node, size=72)!
epilogue → b #0xe848                       ; tail-call to PLT stub 0xe848 (kfree!)
```

**Deferred submission node layout (72B, confirmed):**
| offset | field | notes |
|---|---|---|
| +0x00 | next pointer | circular doubly-linked list |
| +0x08 | prev pointer | |
| +0x10 | desc PA lo | staging word 1 (BAR1+0x4080 or +0x5080) |
| +0x14 | desc PA hi | staging word 2 |
| +0x18 | local_val | staging word 3 (+0x4088 or +0x5088) |
| +0x1C | type (u32) | 0=send channel, 1=batch channel |
| +0x3C | expected completion count (u32) | batch only; written to AB+0x12E4 on re-submission |

**Deferred submission architecture:** submitDataGated enqueues 72B nodes into the per-channel queue (sentinel at AB+0x11F0/AB+0x1200). On each completion IRQ, handleInterrupt pops the next node and re-stages it into BAR1 staging banks + rings doorbell. This is the actual DMA submission path — not direct from userland.

### 12.10 Flag clear / exit @685c
```
[self + ch + 0x11E4] = 0                  ; CLEAR channel active flag (idle!)
if (ch != 1) → EXIT @6958                 ; ch0 done
   [ch==1:] epilogue; b #0xbe64           ; TAIL CALL updateActiveTime()!
```

### 12.11 kernel_debug trace IDs in handleInterrupt
| ID | offset from w22 | location | meaning |
|---|---|---|---|
| 0x2B61FAC | +0 | prologue/thermal/completion entry | main IRQ trace (arg3 = event type: 1=bitfile, 2=thermal, 3=decoder error, 4=status read) |
| 0x2B61FB0 | +4 | batch re-submission @69c0 | deferred node re-staged to batch channel |
| 0x2B61FB4 | +8 | single path @65a8 / drain loop @66e4 | per-completion trace (arg3=old expected count, arg5=_pendingData) |

## 13. AB object layout (offsets from `this`) — accumulated ✅/🟡

| offset | field | notes |
|---|---|---|
| +0x90   | **engine object ptr** (firmware interface) | PAC-verified; vt+0x1E8 = wait-with-callback (§10.1), vt+0x210 = poll-until-deadline (submitDataGated/sleep_timer, frameID=2), **vt+0x208 = completion notify** — `[vt+0x208](engine, ctx, 0)` called by handleInterrupt drain loop (ctx=&AB+0xC0) and single path (ctx=self). Class unknown (§14 open item) |
| +0xA0 / +0xA8 | notifier objects (dmaQ / descDmaQ) | vt+0x150(notifier, obj) on completion (§10/§15.1) — distinct from the +0x90 notifier |
| +0xC0   | **_pendingData** (u32) — in-flight/pending frame counter | kext-maintained: `++` by submitDataGated after flow control; `--` by handleInterrupt as fw completes frames; cleared by reset(); returned to userland by cardStatus(). cleanUp log names it `_pendingData %u`. (Earlier "engine-maintained status word" note superseded.) |
| +0xC8   | _frameNum (u64) — frame counter | used in done-verify `([C8] & ~[12D0]) == [12D8]`; saved to 0x1308 by reset(); cleanUp log names it `_frameNum %llu` |
| +0xD8   | IRQ-stamp high component (u64) | `[AB+0xD8]<<16` in handleInterrupt stamp calc; cleared by reset() |
| +0xE0 / +0xE8 | acc_E0 (active) / acc_E8 (idle) time accumulators | §11 |
| +0xF8   | last-sample timeval | §11 |
| +0x108  | scratch timeval (microtime target) | §11 |
| +0x128  | **bitfile directory table base ptr** (u64) | u32 section-header array walked by handleInterrupt PR path (§12.2): entry = {type@bits[7:0], next_idx_delta@bits[29:14]}; type==1 → decoder section, type==0 → terminator |
| +0x158  | **decoder section pointer** (u64) — points INTO BAR0 at the decoder section found by the §12.2 walk | Section layout: [+0]=header word (live: 0x000b0101), [+4]=enable/disable (0=disable, 1=enable; written by PR cycle + stop()), [+0x10]=frame index/status word (>>16 = completion idx in drain loop; saved to AB+0x12DC on hard error), [+0x14]=frame-done/error word (& 0x1F mask gates the §12.6 decoder-error check). NULL when no decoder section found |
| +0x160  | **thermal distress bitmask** (u32) | §12.5: rising bits OR'd in, falling bits AND-NOT'd out; non-zero → submitFrame drops frames; "entering/exiting thermal distress state" logs on transitions |
| +0x11B4 | **completion-pending flag** (byte) | SET=1 by AB::submitResource@8b74 (firmware resource submission); CLEARED at 60dc in handleInterrupt PR path; gates the §12.6 completion section (`ldrb w8,[self+0x11B4]`). x24 = self+0x11B4 is the base register for this field group in handleInterrupt |
| +0x11C0 / +0x11C8 | **dmaQ / descDmaQ queue OBJECT pointers** (u64) | CORRECTION: object pointers, NOT raw notify registers. PR path (§12.3) tears down DMA state via vtable calls on these objects and nulls them; sub-object retrieved via [vt+0x158] |
| +0x11D0 | decoder-found flag (byte) | set by §12.2 bitfile walk when a type==1 section is found; checked by AB::stop@0xc0a4 before disabling the decoder |
| +0x11E4 / +0x11E5 | **channel active flags** (bytes, ch0/ch1) | addressed as `[self+ch+0x11E4]` — NO index shift (verified raw bytes @9a68/6760). Set by submitDataGated on first submission to a channel; re-set by handleInterrupt when completions leave pending work; cleared by handleInterrupt after the last node is processed. **NOT** the 0x12C8 guard flag (earlier misattribution corrected) |
| +0x11F0 / +0x1200 | per-channel queue head slots (stride 0x10: qbase = self+ch·16) | `[qbase+0x11F0]` = head node ptr (self-ref sentinel when empty); `[qbase+0x11F8]` = tail. Nodes are **sDmaConfig structs (72B)** — pending-submission queue, not raw descriptor nodes: submitDataGated links cfg in on coalesce; handleInterrupt dequeues as fw completes; reset() frees leftovers |
| +0x12C0 | IOLock* — space pool lock | §6 |
| +0x12D0 | **overrun limit cursor** (u64) | hard-error path (§12.6): = [AB+0x1308] + ([decoder+0x10]>>16) + ([AB+0xD8]<<16); used by done-verify `([C8] & ~[12D0]) == [12D8]`; also written by stub first-timeout; = -1 after reset() |
| +0x12D8 | _numFrames (u32) — failure counter | cleared on hard decoder error (§12.6 @64f4), first timeout, and reset() |
| +0x12DC | **offending frame index** (u32) | = [decoder+0x10] saved by hard-error path (§12.6 @64d8); logged via "Offending frameNum" fmt@0x2fd3 |
| +0x12E0 | idle-flag byte | §11 |
| +0x12C8 | **completion-wait guard flag** (byte) | SET by queryCompletionGated@b7b4 on first timeout (also records expected stamp); **SET on hard decoder error @64d0 (§12.6)** — enables overrun check for subsequent completions; CLEARED only by reset()@b0c0 (`strb wzr,[base,#0xe4]`, base=self+0x11E4). READ by submitDataGated (rejects 0xE000_02BC if set) and cleanUp. All four direct `#0x12c8` refs in __text are loads; writes use computed-base patterns |
| +0x12E4 | batch counter mirror (u32) | written by submitDataGated ch1 path: `[self+0x12E4] = [cfg+0x3C]`; consumed by handleInterrupt IRQ completion loop |
| +0x12E8 / +0x12F0 | space pool used / cap (u64) | §6 |
| +0x1308 | IRQ-stamp base = saved [C8] at reset() time | handleInterrupt: `[12D0] = [1308] + ((w8>>16)) + ([D8]<<16)` |

**Reconfig field group (added by §18 decode):** +0x138 firmware status block ptr, +0x150 SPI controller object, +0x169/+0x16A reconfig flags (deferred power-ack), +0x16C flash-update attempt counter, +0x170 ext-config blob pending flag, +0x174 4KB PCI-ext-config blob buffer, +0x1178/+0x1188 commit objects D/E (vt+0x1F0), +0x1198/+0x11A0 primary/secondary SPI buffer objects — full table in §18.6.

## 14. Open items / next steps

1. ✅ DONE this session: queryCompletion@0xb88c (§10), cardStatus@0xbd90, getPercent@0xbda0, unbindFrame@0xbf6c, bindFrame(desc)@0xbec, updateActiveTime@0xbe64, updateIdleTime@0x9be0 — all in §6
2. ✅ DONE: handleInterrupt full body — complete decode in §12 (bitfile walk, thermal distress, PR DMA reset, completion dispatch, drain loop, deferred re-submission, hard-error path); sleep_timer summary in §17
3. ✅ DONE: AB::reset @0xaea8 (§10.3) — per-frame teardown sequence fully decoded
4. 🟡 bindFrame slot-selection logic (dd9c..de8c): auto-assign vs explicit index via in[+0x18]; 0xFF cap meaning
5. ✅ DONE: AB::cleanUp @0xb0ec (§15.1) + AB::submitDataGated @0x985c (§15.2) — submission core fully decoded; guard-flag lifecycle resolved (see §13); sContext queue layout mapped (§15.3)
6. 🟡 `[desc_vt+0x1F8](desc, 1)` semantics on submit success (note: cleanUp calls vt+0x1F8(rec, 0) on dmaQ completion records — same slot offset, different receiver; see §15.1)
7. 🟡 IODirection enum mapping for withAddressRange dir=1/2
8. ✅ DONE: lifecycle overrides — full decode in §19 (start/stop/clientClose/clientDied/willTerminate/didTerminate/terminate/finalize/getMetaClass/D1Ev/D0Ev); none touch HW directly; stop() is the teardown template for the Linux driver
9. 🟡 UC+0x1900 field semantics; s_inputDataStruct +0x14/+0x18 fields
10. ⬜ Engine callback table (§16): 62 entries decoded & demangled — identify which are actually dispatched by firmware (the three *Gated callbacks) vs dead/legacy; spiprogram@821c + loadBitfile@8628 now decoded (§18.7), submitResource still open
11. 🟡 sContext queue PUSH side: who links 24B {next,prev,desc} nodes into the four §15.3 queues (handleInterrupt / submitFrame path) — only pop/drain decoded so far
12. ⬜ Engine object class (AB+0x90): now 4 known vt slots — 0x1E8 wait-with-callback (§10.1), 0x208 completion notify (§12.7/§12.8, ctx = self or &AB+0xC0; also &AB+0x12F8 wake-gate address from setPowerState tail §20.3), 0x210 poll-until-deadline (frameID=2). **Second engine object at AB+0x98** with vt+0x180(eng) key 0x8bec (§20.3). Identify the class(es); needed to replicate notify semantics in the Linux driver
13. ⬜ BAR1+0x2044 bit layout: bit0=valid confirmed live (Phase 3i); bit1=channel selector inferred from dispatch (@63d4 compares status>>1 against 0/1). Higher bits unverified — may carry completion count
14. ⬜ [decoder+0x14] frame-done/error word: & 0x1F mask gates §12.6 decoder-error check; bit0=hard error (sets guard flag + overrun limit); bits 1–4 firmware-defined, meaning unknown
15. ⬜ AB::submitResource @0x8a1c–0x8d50 full body: only the AB+0x11B4=1 write at 0x8b74 confirmed; what resources are submitted and in what order is still open
16. ✅ DONE: reconfig_handler @0x6b94 — full decode in §18 (firmware image-type state machine: golden/HWTE/live/debug/unknown; SPI flash update path with 2×20MB channel programming, attempt counter, lazy bitfile load `live_2.6X_0x01_{primary,secondary}[_ecXX].bin`; commit vcall vt+0x1F0 + BAR0[+0x2004] re-enable words 0x00019000/0x0001A000; PCI ext-config 4KB blob flush). Helpers decoded: change_pointer@8580, spierase@809c, loadBitfile@8628, reconfig_timer@74c4
17. ⬜ Who fills AB+0x174 (4KB ext-config blob)? No __text writer — presumed firmware DMA during the vt+0x1F0 commit vcall (§18.9)
18. ⬜ Commit objects D/E/F ([self+0x1178]/[self+0x1188]/**[self+0x1300]**) class + vt+0x1F0("commit", timeout=0x3E8, key 0x246e) contract — F added by setPowerState(0) power-down path (§20.3/§20.4); vt+0x1F8 on [self+0x1180] (reconfig_timer poll) and **[self+0x1190]** (pointer_restore_timer, key 0xa28c §20.1) status layout
19. 🟡 spiprogram@821c x4/w5 argument semantics — both call sites now fully documented (§20.5): change_pointer = (ch, off=0x13FFF40, src=64B static SPI command table, w4=0x40, w5=0); reconfig third_phase = (ch, size=0x1400000, src=image data from vt+0x308, dst=vt+0x300, w5=1). Internal interpretation of x4/w5 still open

## 15. Submission core deep dive — cleanUp + submitDataGated (2026-11-05)

### 15.1 AB::cleanUp @0xb0ec–0xb634 ✅ (`tmp/dis_ab_cleanUp.txt`)

Signature: `void AB::cleanUp(sContext* ctx)` — x0=self, x1=ctx (sContext at UC+0x18F8).
Finalizes all in-flight work before teardown (called from the unbindAll path).

**Flow:**

```c
void AB::cleanUp(sContext* ctx) {
    // 1. Engine wait — only if not already cleaned ([ctx+0x60] bit31 clear)
    if (!([ctx+0x60] & (1u<<31))) {
        [engine_vt+0x1E8](engine, pacia(stub@0xb634), 0, &u32(=0), 0, 0);
        // engine wait; PAC'd callback = queryCompletionGated stub (same as §10.1)
    }

    // 2. Decoder-error check — guard flag AB+0x12C8 set ⇒ a completion wait timed out
    if ([self+0x12C8]) {
        IOLog("AppleAfterburner Error: %s[%p]::%s: Decoder error (resetting) "
              "_frameNum %llu, completionStamp %lld, _pendingData %u, status 0x%08x\n",
              getName(self), self, "cleanUp", [C8], [1270], [C0], [12DC]);   // fmt@0x2A05
        [engine_vt+0x1E8](engine, pacia(AB::reset@0xaea8), 0, 0, 0, 0);
        // engine-driven reset: PAC'd callback = AB::reset itself (§10.3)
    }

    // 3. Drain all four sContext completion queues (layout §15.3)
    for (iter = 0; iter < 2; iter++) {
        drainQueue(&ctx->qA[iter], lock=[ctx+iter*8+0x40], notifier=[self+0xA0] /*dmaQ*/,
                   recAction = iter==0 ? vt+0x1F8(rec,0) : vt+0x220(rec,0)+delete);
        drainQueue(&ctx->qB[iter], lock=[ctx+iter*8+0x50], notifier=[self+0xA8] /*descDmaQ*/,
                   recAction = vt+0x220(rec,0)+delete);   // qB always destroys records
    }
}

void drainQueue(head_slot, IOLock* lock, notifier, recAction) {
    while ((node = *head_slot) != head_slot) {          // self-ref sentinel = empty
        IOLockLock(lock);
        node = *head_slot;                              // re-check under lock
        if (node == head_slot) { IOLockUnlock(lock); break; }
        next = node->next;                              // +0x00
        next->prev = head_slot;                         // +0x08
        *head_slot = next;
        IOLockUnlock(lock);

        desc = node->desc;                              // +0x10 (IOMemoryDescriptor*)
        rec  = [desc_vt+0x158](desc);                   // gate call — record ptr or null
        if (!rec) continue;                             // nothing pending for this node
        rec  = [desc_vt+0x158](desc);                   // second call → object ptr (x22)
        [desc_vt+0x150](desc, 1);                       // release mark on desc
        [notifier_vt+0x150](notifier, desc);            // completion notification
        IOFree(node, 0x18);                             // node = 24B {next, prev, desc}
        recAction(rec);                                 // qA iter0: vt+0x1F8(rec,0) only; else vt+0x220 + virtual delete via [rec+0x28]
    }
}
```

**Key facts:**
- Node size **24B (0x18)** — confirmed by `IOFree(node, 0x18)` @b390/b5f0. These are NOT the sDmaConfig nodes of the AB+0x11F0/0x1200 pending-submission queues (§13) — two distinct queue systems:
  - **sContext completion queues** (4×, this section): 24B {next, prev, IOMemoryDescriptor*} wrappers; drained per-completion by queryCompletion (§10) and finally here.
  - **AB pending-submission queues** (2×, AB+0x11F0/+0x1200): sDmaConfig nodes (~72B); managed by submitDataGated/handleInterrupt/reset.
- `[desc_vt+0x158](desc)` is called **twice per node**: first as a gate (null → skip), second returns the completion-record object. Same slot, presumably side-effecting detach semantics — exact contract open.
- Record handling asymmetry: only qA iter 0 (head slot ctx+0x00) gets `vt+0x1F8(rec, 0)` release-only; all other records get `vt+0x220(rec, 0)` + virtual delete via `[rec+0x28]` destructor vtable.
- The "already cleaned" guard is **bit 31 of [ctx+0x60]** (u64 in sContext) — distinct from the AB+0x12C8 completion-wait guard flag (§13). Who sets ctx+0x60 bit31: open (presumably UC::unbindAll/stop before first cleanUp, or a prior error path).
- Engine wait #1 uses **frameID=0** and a local u32 timeout slot initialized to 0 @b120 — "wait for completion now, no retry budget".

### 15.2 AB::submitDataGated @0x985c–0x9be0 ✅ (`tmp/dis_ab_completion_fn.txt`)

Signature: `uint32_t AB::submitDataGated(sDmaConfig* cfg)` — x0=self, x1=cfg.
**No direct `bl` callers in __text** — reached via the engine callback table (§16, entry 22) when firmware dispatches a submission request. This is the real submission core our Linux driver must replicate.

**Return codes:**

| value | meaning |
|---|---|
| 0 | success — doorbell rung (fresh channel) or coalesced into active batch |
| 0xE000_02D5 | "queued" — cfg linked as pending node behind an active batch; no doorbell this call |
| 0xE000_02BC | error — guard flag set, wake failure (logged), or flow-control timeout ("Possible hang", logged) |

**Flow:**

```c
uint32_t AB::submitDataGated(sDmaConfig* cfg) {
    uint32_t ret = 0xE000_02BC;                 // default: error
    int ch = [cfg+0x20];                        // raw channel index (0/1), no shift

    // 1. Wake-wait — only while wake flag AB+0x12F8 set AND cfg not urgent
    while ([self+0x12F8]) {                     // wake/busy flag byte
        if ([cfg+0x1C]) break;                  // urgent → skip wait entirely
        r = [engine_vt+0x200](engine, &self->wake@AB+0x12F8, 2);   // "in use by firmware?" slot
        if (r == 0) continue;                   // re-check flag
        IOLog("...Wake: 0x%08x", r);            // fmt@0x2326, fn-name 'submitDataGated'@0x2357
        return ret;                             // wake failure → error
    }

    // 2. Guard flag — completion wait in progress? reject
    if ([self+0x12C8]) return ret;              // set by queryCompletionGated timeout (§13)

    // 3. Flow control — pending frames >= 32? poll engine until space or deadline
    while ([self+0xC0] /*_pendingData*/ > 31) {
        if ([cfg+0x1C]) break;                  // urgent → skip wait
        clock_interval_to_deadline(0x7D0, 0xF4240, &deadline);   // interval=2000, max=1,000,000 ticks
        r = [engine_vt+0x210](engine, &_pendingData@AB+0xC0, deadline /*by value*/, 2);
        if (r != 0) {
            if (r == 1) IOLog("...Possible hang, frameNum %llu, completionStamp %lld\n",   // fmt@0x2367
                              [C8], [1270]);
            return ret;                         // timeout → error (frame NOT submitted)
        }
    }

    // 4. Completion word — tell firmware what stamp to echo back (skipped if urgent)
    if (!([cfg+0x1C]) && ([self+0x158] /*decoder ctrl*/)) {
        u32 word = (([C8] - [1308]) << 16)      // _frameNum minus IRQ-stamp base
                 | ([cfg+0x38] << 8)
                 | ([cfg+0x34] << 4)
                 |  [cfg+0x30];
        [decoder_ctrl+0xC] = word;              // ← the completion signal missing in Phase 3l (BAR0[hdr+0xC])
    }
    [self+0xC0]++;                              // _pendingData++

    // 5. Queue push / coalesce (per-channel pending-submission queue, §13)
    qbase = self + ch*16;                       // head slot at qbase+0x11F0, tail at qbase+0x11F8
    if ((head = [qbase+0x11F0]) != &slot) {     // non-empty → coalesce into active batch
        ext = [cfg+0x40];                       // ext struct ptr
        [ext+0x18] = [cfg+0x10];                // staging word A → ext slot
        flags = [ext]; flags &= ~1; [ext] = flags;   // clear pending bit
        [head+0x3C]++;                          // batch counter++ on first node
        return 0;                               // success, no doorbell
    }

    flag_addr = self + ch + 0x11E4;             // channel active flag (NO shift!)
    if ([flag_addr]) {                          // flag set but queue empty → pending-node path
        tail = [qbase+0x11F8];
        cfg[+0] = &slot; cfg[+8] = tail;        // link into list
        *tail = cfg;                            // old_tail.next = cfg
        [qbase+0x11F8] = cfg;                   // update tail
        return 0xE000_02D5;                     // "queued" — fw picks up when current batch done
    }

    // 6. First submission to this channel: set flag + ring doorbell
    [flag_addr] = 1;
    if (!([cfg+0x1C])) AB::updateIdleTime();    // idle→active bookkeeping (gated by urgent)
    BAR1 = [self+0x130];
    if (ch == 0) {                              // h2c / send channel
        [BAR1+4] = 0;                           // clear-before-ring!
        [BAR1+0x4080] = [cfg+0x10];             // staging: word A
        [BAR1+0x4084] = [cfg+0x14];             // staging: word B
        [BAR1+0x4088] = [cfg+0x18];             // staging: word C
        [BAR1+4] = 0xFFE7F;                     // RING h2c doorbell
    } else if (ch == 1) {                       // c2h / batch channel
        [BAR1+0x1004] = 0;                      // clear-before-ring!
        [BAR1+0x5080] = [cfg+0x10];
        [BAR1+0x5084] = [cfg+0x14];
        [BAR1+0x5088] = [cfg+0x18];
        [BAR1+0x1004] = 0x80FFE7F;              // RING batch doorbell
        [self+0x12E4] = [cfg+0x3C];             // mirror batch counter for IRQ handler
        kernel_debug(0x2B61FB0, self, [cfg+0x3C], 1/*ch*/, [self+0xC0]/*pending*/, 0);
    } else {                                    // defensive: ch ∉ {0,1} → no doorbell
    }
    return 0;
}
```

**Key facts:**
- **cfg[+0x1C] = "urgent / no-completion" flag**: when non-zero it skips wake-wait, flow-control wait, completion-word write, and updateIdleTime. Exact semantics open (presumably set by firmware for internal/resource submissions).
- **Completion word @ [decoder_ctrl+0xC]**: `(frameNum - stampBase) << 16 | cfg[+0x38]<<8 | cfg[+0x34]<<4 | cfg[+0x30]`. This is the frame-done signal firmware echoes — the write our Linux driver was missing in Phase 3l (we never wrote BAR0[hdr+0xC]).
- **Flow-control poll** passes `&_pendingData` as x1 and deadline by value; constant w3=2. Distinct from queryCompletionGated's vt+0x210 use (§10.1).
- **Wake slot vt+0x200** takes the wake-flag address by reference + constant 2 — same "is frame in use by firmware?" slot documented for IOMemoryDescriptor (§6), here on the engine object.
- Doorbell staging words: cfg[+0x10/+0x14/+0x18] → BAR1[+0x4080..C] (ch0) / [BAR1+0x5080..C] (ch1). Matches Phase 3o live findings exactly.
- **kernel_debug trace** on ch1 ring: ID 0x2B61FB0, args (self, batchCounter, 1, pendingData, 0).

### 15.3 sContext layout (at UC+0x18F8) — from cleanUp + queryCompletion ✅

Both functions use identical addressing (`iter*8` for locks, `iter*16` / `+0x20` for head slots), confirming a shared four-queue structure:

| offset | field | notes |
|---|---|---|
| +0x00 | qA[0] head slot (self-ref sentinel) | notifier dmaQ [AB+0xA0]; rec → vt+0x1F8(rec,0) release-only |
| +0x10 | qA[1] head slot | notifier dmaQ; rec → vt+0x220 + virtual delete |
| +0x20 | qB[0] head slot | notifier descDmaQ [AB+0xA8]; rec → vt+0x220 + virtual delete |
| +0x30 | qB[1] head slot | notifier descDmaQ; rec → vt+0x220 + virtual delete |
| +0x40 / +0x48 | IOLock* for qA[0] / qA[1] | loaded at [ctx+iter·8+0x40] |
| +0x50 / +0x58 | IOLock* for qB[0] / qB[1] | loaded at [ctx+iter·8+0x50] |
| +0x60 | u64; **bit 31 = "already cleaned" guard** | gates cleanUp's engine wait (§15.1) |

Node (24B, IOFree size 0x18): `{+0x00 next, +0x08 prev, +0x10 IOMemoryDescriptor*}`.
Drained per-completion by queryCompletion (§10: notifier loads @ba50/bcc0) and finally by cleanUp. Push side (handleInterrupt / submitFrame path) not yet decoded — open item 11.

**Channel mapping of the four queues is an open question** — iter 0/1 may index channels or h2c/c2h directions; the notifier split (dmaQ vs descDmaQ) follows the queue pair, not the iteration.

## 16. Engine callback table @0x1d6a0–0x1da7f ✅ (`tmp/engine_callback_table.md`)

62 entries × 16B = {fn_ptr(8B), selector(8B: upper32=class_tag, lower32=method_id)}.
Located in **`__LINKEDIT`** (confirmed via pm2.py section parse) — no direct code references; consumed externally (Mach-O section walk / framework dispatch by the engine firmware bridge).

Class tags observed: `0x30f` (58 entries), `0xb0f` (2: submitData, freeSpace), `0x60f` (1: cardStatus), plus 2 MetaClass ctor entries.

Full demangled table (custom Python Itanium demangler; Apple 'y'=u64 extension):

| # | addr | fn_ptr | name | class_tag | method_id |
|---|---|---|---|---|---|
| 0 | `0x1d6a0` | `0xbd90` | AppleAfterburner::cardStatus(u64 *) | `0x60f` | `0x2a` |
| 1 | `0x1d6b0` | `0x10198` | ?? 0x10198 (data) | `0x30f` | `0x4e` |
| 2 | `0x1d6c0` | `0xbda0` | AppleAfterburner::getPercent(u64 *) | `0x30f` | `0x74` |
| 3 | `0x1d6d0` | `0x821c` | AppleAfterburner::spiprogram(uint32, uint32, void *, u64ll, char) | `0x30f` | `0x9e` |
| 4 | `0x1d6e0` | `0xa67c` | AppleAfterburner::submitData(sContext *, IOMemoryDescriptor *, uint32, u64 *, uint32, uint32, uint32, u64, u64) | `0xb0f` | `0xe9` |
| 5 | `0x1d6f0` | `0x141e0` | ?? 0x141e0 (data) | `0x30f` | `0x10d` |
| 6 | `0x1d700` | `0xa28c` | AppleAfterburner::getSegments(sContext *, IOMemoryDescriptor *, IODMACommand::Segment64 *, uint32 *, u64, u64, u64) | `0x30f` | `0x171` |
| 7 | `0x1d710` | `0x8628` | AppleAfterburner::loadBitfile(uchar const *, uint32) | `0x30f` | `0x19a` |
| 8 | `0x1d720` | `0x6a00` | AppleAfterburner::sleep_timer(IOTimerEventSource *) | `0x30f` | `0x1d4` |
| 9 | `0x1d730` | `0xa9ac` | AppleAfterburner::submitFrame(sContext *, IOMemoryDescriptor *, (subst), u64 *, uint32, uint32, uint32, sDescriptorLengthOffsetInfo) | `0x30f` | `0x23d` |
| 10 | `0x1d740` | `0xbf6c` | AppleAfterburner::unbindFrame(IOMemoryDescriptor *) | `0x30f` | `0x277` |
| 11 | `0x1d750` | `0xca80` | AppleAfterburner::didTerminate(IOService *, uint32, char *) | `0x30f` | `0x2ab` |
| 12 | `0x1d760` | `0xbfec` | AppleAfterburner::checkForSpace(u64 *) | `0x30f` | `0x2d4` |
| 13 | `0x1d770` | `0x7c60` | AppleAfterburner::setPowerState(u64ll, IOService *) | `0x30f` | `0x307` |
| 14 | `0x1d780` | `0xca68` | AppleAfterburner::willTerminate(IOService *, uint32) | `0x30f` | `0x33a` |
| 15 | `0x1d790` | `0x8580` | AppleAfterburner::change_pointer(uint32) | `0x30f` | `0x363` |
| 16 | `0x1d7a0` | `0x74c4` | AppleAfterburner::reconfig_timer(IOTimerEventSource *) | `0x30f` | `0x3a0` |
| 17 | `0x1d7b0` | `0x8af0` | AppleAfterburner::submitResource(IOBufferMemoryDescriptor *, uint32) | `0x30f` | `0x3e4` |
| 18 | `0x1d7c0` | `0x9be0` | AppleAfterburner::updateIdleTime(void) | `0x30f` | `0x40d` |
| 19 | `0x1d7d0` | `0x5aec` | AppleAfterburner::handleInterrupt(OSObject *, IOInterruptEventSource *, int) | `0x30f` | `0x45a` |
| 20 | `0x1d7e0` | `0xb88c` | AppleAfterburner::queryCompletion(sContext *, u64 *, (subst)) | `0x30f` | `0x492` |
| 21 | `0x1d7f0` | `0x803c` | AppleAfterburner::readDMARegister(uint32) | `0x30f` | `0x4bc` |
| 22 | `0x1d800` | `0x985c` | AppleAfterburner::submitDataGated(sDmaConfig *) | `0x30f` | `0x4f2` |
| 23 | `0x1d810` | `0x55c0` | AppleAfterburner::readBAR0Register(uint32) | `0x30f` | `0x51d` |
| 24 | `0x1d820` | `0x6b94` | AppleAfterburner::reconfig_handler(IOInterruptEventSource *, int) | `0x30f` | `0x561` |
| 25 | `0x1d830` | `0xbe64` | AppleAfterburner::updateActiveTime(void) | `0x30f` | `0x58c` |
| 26 | `0x1d840` | `0x8030` | AppleAfterburner::writeDMARegister(uint32, uint32) | `0x30f` | `0x5b8` |
| 27 | `0x1d850` | `0x9c5c` | AppleAfterburner::getDescriptorList(sContext *, IODMACommand::Segment64 *, uint32, uint32, u64 *, uint32 *, u64) | `0x30f` | `0x60f` |
| 28 | `0x1d860` | `0x8084` | AppleAfterburner::readFlashRegister(uint32) | `0x30f` | `0x63b` |
| 29 | `0x1d870` | `0x8024` | AppleAfterburner::writeBAR0Register(uint32, uint32) | `0x30f` | `0x668` |
| 30 | `0x1d880` | `0x5ad4` | AppleAfterburner::readDecodeRegister(uint32) | `0x30f` | `0x695` |
| 31 | `0x1d890` | `0x8090` | AppleAfterburner::writeFlashRegister(uint32, uint32) | `0x30f` | `0x6c3` |
| 32 | `0x1d8a0` | `0x5a3c` | AppleAfterburner::configureInterrupts(void) | `0x30f` | `0x6f1` |
| 33 | `0x1d8b0` | `0x8d5c` | AppleAfterburner::getResourceSegments(IOBufferMemoryDescriptor *, IODMACommand::Segment64 *, uint32 *, u64) | `0x30f` | `0x757` |
| 34 | `0x1d8c0` | `0x806c` | AppleAfterburner::readBitfileRegister(uint32) | `0x30f` | `0x785` |
| 35 | `0x1d8d0` | `0x8054` | AppleAfterburner::readThermalRegister(uint32) | `0x30f` | `0x7b3` |
| 36 | `0x1d8e0` | `0x55cc` | AppleAfterburner::readVersionRegister(uint32) | `0x30f` | `0x7e1` |
| 37 | `0x1d8f0` | `0x9698` | AppleAfterburner::submitResourceGated(sDmaConfig *) | `0x30f` | `0x81b` |
| 38 | `0x1d900` | `0x8014` | AppleAfterburner::writeDecodeRegister(uint32, uint32) | `0x30f` | `0x84a` |
| 39 | `0x1d910` | `0xb634` | AppleAfterburner::queryCompletionGated(u64 *, int *) | `0x30f` | `0x87c` |
| 40 | `0x1d920` | `0x87c8` | AppleAfterburner::resourceLoadCallback(uint32, int, void const *, uint32, void *) | `0x30f` | `0x8b2` |
| 41 | `0x1d930` | `0x8078` | AppleAfterburner::writeBitfileRegister(uint32, uint32) | `0x30f` | `0x8e2` |
| 42 | `0x1d940` | `0x8060` | AppleAfterburner::writeThermalRegister(uint32, uint32) | `0x30f` | `0x912` |
| 43 | `0x1d950` | `0x8048` | AppleAfterburner::writeVersionRegister(uint32, uint32) | `0x30f` | `0x942` |
| 44 | `0x1d960` | `0x7c1c` | AppleAfterburner::pointer_restore_timer(IOTimerEventSource *) | `0x30f` | `0x986` |
| 45 | `0x1d970` | `0x7508` | AppleAfterburner::pointer_restore_handler(IOInterruptEventSource *, int) | `0x30f` | `0x9d1` |
| 46 | `0x1d980` | `0x90d0` | AppleAfterburner::getResourceDescriptorList(IODMACommand::Segment64 *, uint32, uint32, u64 *, uint32 *, u64) | `0x30f` | `0xa26` |
| 47 | `0x1d990` | `0x55d8` | AppleAfterburner::updateIORegistryProperties(void) | `0x30f` | `0xa5b` |
| 48 | `0x1d9a0` | `0x888c` | AppleAfterburner::resourceLoadCallbackHandler(uint32, int, void const *, uint32) | `0x30f` | `0xa96` |
| 49 | `0x1d9b0` | `0xc984` | AppleAfterburner::free(void) | `0x30f` | `0xab4` |
| 50 | `0x1d9c0` | `0xc8c0` | AppleAfterburner::init(OSDictionary *) | `0x30f` | `0xae0` |
| 51 | `0x1d9d0` | `0xc0a4` | AppleAfterburner::stop(IOService *) | `0x30f` | `0xb08` |
| 52 | `0x1d9e0` | `0xc998` | AppleAfterburner::probe(IOService *, int *) | `0x30f` | `0xb33` |
| 53 | `0x1d9f0` | `0xaea8` | AppleAfterburner::reset(void) | `0x30f` | `0xb52` |
| 54 | `0x1da00` | `0x4254` | AppleAfterburner::start(IOService *) | `0x30f` | `0xb7b` |
| 55 | `0x1da10` | `0xb0ec` | AppleAfterburner::cleanUp(sContext *) | `0x30f` | `0xba5` |
| 56 | `0x1da20` | `0xcab0` | AppleAfterburner::finalize(uint32) | `0x30f` | `0xbc7` |
| 57 | `0x1da30` | `0x809c` | AppleAfterburner::spierase(uint32) | `0x30f` | `0xbe9` |
| 58 | `0x1da40` | `0x4000` | MetaClass::ctor() | `0x30f` | `0xc0e` |
| 59 | `0x1da50` | `0x40f8` | MetaClass::ctor() | `0x30f` | `0xc33` |
| 60 | `0x1da60` | `0xbedc` | AppleAfterburner::bindFrame(IOMemoryDescriptor *) | `0x30f` | `0xc6a` |
| 61 | `0x1da70` | `0xc058` | AppleAfterburner::freeSpace(u64 *) | `0xb0f` | `0xc8e` |

**Notable for the Linux driver:**
- Entry 22 = submitDataGated — firmware dispatches submissions through this table; our driver's fast path must implement the same contract (§15.2).
- Entries 39/37 = queryCompletionGated / submitResourceGated — the three "*Gated" callbacks are the firmware-facing gated variants (vs plain queryCompletion@0xb88c, submitData@0xa67c).
- New methods not previously decoded: **spiprogram**@0x821c, **loadBitfile**@0x8628, **submitResource**@0x8af0, **resourceLoadCallback(Handler)**@0x87c8/0x888c, register accessors (read/write BAR0/DMA/Flash/Thermal/Version/Decode/Bitfile @0x55cc–0x809c), pointer_restore_handler/timer @0x7508/0x7c1c, change_pointer@0x8580.
- Entries 1 & 5 point to data (0x10198, 0x141e0) — not functions; likely table metadata or vtable refs.

## 17. AB::sleep_timer @0x6a00–0x6b90 ✅ (IOTimerEventSource callback — hang watchdog)

Decoded from `tmp/dis_ab_handleInterrupt.txt` (dump covers 0x5aec–0x6b94; sleep_timer is the
second function in that range, with its own prologue @6a00). PLT targets resolved via
`static_re/stub_map.json` + `imports_map.json`.

**Role:** periodic hang-watchdog. While frames are pending it polls the engine for a stuck
frame (deadline-bounded); on detection it logs + calls AB::reset() and keeps polling until
_pendingData drains to zero. On exit it performs reset bookkeeping (stamp-base save, high
clear) and **disables the decoder**.

```c
void AB::sleep_timer(IOTimerEventSource* src) {   // x0 = self (IOService convention)
    u32 pending = [self+0xC0];                    ; _pendingData (post-increment load; x20 now = &AB+0xC0)
    const char* hang_str = 0x2C69;                ; "hang" log string
    if (pending == 0) goto cleanup;

loop:                                             // @6a38 — hang-check loop
    u64 out = 0;                                  ; stack slot, passed by ref to engine poll
    deadline = clock_interval_to_deadline(0x7D0, 0xF4240);   // PLT _clock_interval_to_deadline (idx 45)
    r = [engine_vt+0x210](engine=[self+0x90], self, &out, /*frameID=*/2);   // poll-until-deadline!
    if (r != 1) goto recheck;                     // no hang this round

    // --- HANG DETECTED (r == 1) @6a9c ---
    name = [self_vt+0x3A8](self, 0);              // getName-style vcall (vt slot 149)
    IOLog(fmt@0x2367, name, _frameNum=[self+0xC8], last_cursor=[self+0x1270], r);
    AB::reset();                                  // ← full reset on hang! (bl #0xaea8)

recheck:                                          // @6afc
    if ([self+0xC0] != 0) goto loop;              // keep polling while pending data remains

cleanup:                                          // @6b04 — no pending, or drain complete
    [self+0x1308] = [self+0xC8];                  // save _frameNum → IRQ-stamp base (same field reset() saves)
    [self+0xD8]   = 0;                            // clear IRQ-stamp high component
    if ([self+0x158])                             // decoder section present?
        [decoder_section+4] = 0;                  // **DECODER DISABLE** on timer exit! (BAR0[hdr+4]=0)
    name = [self_vt+0x3A8](self, 0);
    dec_state = [decoder_section+4];              // read back enable state (0 if no section)
    IOLog(fmt@0x1B48, name, self, hang_str, dec_state);   // final log with decoder state
    IOService::acknowledgeSetPowerState(self);    // PLT idx 35 — power-management wake cleanup!
}
```

**Key facts:**
- Engine poll: `[AB+0x90]->[vt+0x210](engine, self, &out, frameID=2)` — same vt slot as submitDataGated's flow-control poll; **return 1 = hang detected**.
- Hang response is a full `AB::reset()` (not just a log) — the watchdog can reset the card autonomously.
- Loop condition: `[self+0xC0]` (_pendingData) non-zero → re-poll. The timer keeps firing until all pending frames are accounted for.
- Cleanup path writes **the same fields as reset()**: AB+0x1308 (stamp base = saved _frameNum) and AB+0xD8 (=0). So even a no-hang timer expiry refreshes the stamp baseline.
- **Decoder disable on exit** (`[decoder_section+4] = 0`) — third known write site for BAR0[hdr+4] alongside stop()@c0a4 and the PR re-enable cycle (§12.2). Confirms decoder-off is standard teardown state.
- Ends with `IOService::acknowledgeSetPowerState()` — power-management bookkeeping (timer fired during a power-state transition).

**Linux driver implications:** replicate as a periodic workqueue timer: if pending>0, poll engine status with deadline; on stuck frame → log + full reset path; on exit always refresh stamp base and ensure decoder disabled.

## 18. AB::reconfig_handler @0x6b94–0x74ac ✅ (firmware image-type state machine) + reconfig_timer @0x74c4

Decoded from `tmp/dis_reconfig_handler.txt` (full disasm 0x6b94–0x821c). Function spans
**0x6b94–0x74ac** (~5.8KB) with secondary blocks placed after the epilogue @6efc–6f2c
(compiler cold-block layout; all paths converge on tail `@6efc → retab @6f2c`).
Names from §16 engine-callback table: reconfig_handler (this fn), **reconfig_timer**@0x74c4,
**change_pointer**@0x8580, **spierase**@0x809c, **loadBitfile**@0x8628, **spiprogram**@0x821c.
No direct `bl` callers — both reconfig fns are vtable-dispatched (IOKit event-source /
engine-callback registration).

### 18.1 Role

Board-level firmware reconfiguration state machine. Reads the current image type from the
firmware status block (**AB+0x138**, see §18.6) and drives one of: golden flash update,
live→golden pointer restore, or no-op log. The SPI-flash path programs **two 20MB channel
images** (primary/secondary) loaded from the kext bundle via `OSKextRequestResource`, then
commits through a firmware "commit" vcall and re-enables the engine with a reconfig-specific
BAR0[+0x2004] word.

### 18.2 Entry — ext-config blob flush @6b94–6c5c

```c
void AB::reconfig_handler(void) {          // x0 = self; vtable-dispatched (no bl callers)
    if ([self+0x170]) {                    ; reconfig-blob-pending flag (byte)
        for (off = 0; off < 4096; off += 4)
            IOPCIDevice::extendedConfigWrite32([self+0x88], off, [self+0x174 + off]);
                                            ; ← writes a 4KB blob from AB+0x174 into PCI
                                              ;   EXTENDED config space (1024 × u32)
        [self+0x170] = 0;                  ; clear flag after flush
    }

    w24 = [[self+0x138]+4];                ; board-revision word (image type in bits[23:16])
    name = getName();                      ; vcall vt+0x3A8(self, 0)
    IOLog(fmt@abs0x979 "board revision: 0x%08x", name, self, "reconfig_handler"@0x160C, w24);

    w25 = [[self+0x138]+8];                ; version word
    x20 = w25 >> 16;                       ; major (u16)
    w23 = w25 & 0xFFFF;                    ; minor (u16)

    switch ((w24 >> 16) & 0xF) {           ; jump table @0x74B4, 5×s32 PC-rel to base_pc=0x6ce8
```

### 18.3 Case 0 — GOLDEN image @6cf0–6d90

```c
    case 0:   // "image type is golden" (fmt@0x161D)
        name = getName(); IOLog(fmt@0x161D, name, self);
        if ((w25 & (1<<17)) || (minor > 0x18))     ; bit17 of version word OR minor > 24
            goto flash_update_entry @6f48;         ; → attempt-limited SPI update path
        name = getName();
        IOLog(fmt@abs0x9F7 "unsupported golden image %u.%u... bailing out",
              name, self, major, minor);           ; too-old golden: refuse, exit
        goto tail;
```

### 18.4 Cases 1 / 3 / default — log-only @6ddc–6e20

| case | image type | fmt (abs) | action |
|------|-----------|-----------|--------|
| 1    | HWTE      | 0x1832 "image type is HWTE..." | log, tail |
| 3    | debug     | 0x19BB "image type is debug..." | log, tail |
| >3   | unknown   | 0x19F6 "image type is unknown..." | log, tail |

### 18.5 Case 2 — LIVE image @6e24–73dc (the interesting path)

```c
    case 2:   // "live" image
        if ([self+0x16C] > 1) goto reconfig_to_live;      ; attempt counter ≥2 → assume done
        if (major == 2 && minor ∈ {0x3E, 0x3F})           ; already at golden target version!
            goto reconfig_to_live @6f30;

        // --- flash-update path @6e5c (live image NOT yet at 2.62/2.63) ---
        name = getName();
        IOLog(fmt@0x186C "image type is live... reconfiguring to golden for flash update", ...);
        [self+0x170] = 1;                  ; arm ext-config blob flush (consumed next entry)
        x0 = [self+0x1178];                ; commit object D
        goto apply_and_engine_enable @7054;   ; → BAR0[+0x2004] = 0x00019000

reconfig_to_live:                          // @6f30 — "update complete" path
        w20 = (major==2 && minor==0x3E) ? 1 : (major==2 && minor==0x3F);   ; variant flag
        name = getName();
        if (!w20) goto give_up @70a0;      ; neither 2.62 nor 2.63 → give up
        IOLog(fmt@0x18CF "image update is complete!", ...);
        IOLog(fmt@0x190D "image type is live... reconfiguring to golden for pointer restore", ...);
        [self+0x170] = 1;                  ; arm ext-config blob flush
        x0 = [self+0x1188];                ; commit object E (different from D!)

apply_and_engine_enable:                   // @7054 — shared by both live sub-paths
        PAC-verify(x0, tag 0xcda1);
        [obj_vt+0x1F0](obj, w1=0x3E8);     ; "commit" vcall, timeout = 1000 (ms)
        [BAR0+0x2004] = 0x00019000;        ; engine re-enable word (pointer-restore variant)
                                            ;   ([self+0x148]+4 — BAR0 base +0x2000 region,
                                            ;    NOT the BAR1+0x2004=0x16 init enable)
        goto tail;

give_up:                                   // @70a0
        IOLog(fmt@0x1656 "giving up on flash update. max attempts = %u", ..., [self+0x16C]);
        [self+0x169] = 0;                  ; clear byte flag
        if ([self+0x16A]) {                ; deferred power-ack pending?
            IOLog(fmt@0x1973 "deferred ack for power state change", ...);
            IOService::acknowledgeSetPowerState(self);   ; PLT 0xe9a4
            [self+0x16A] = 0;
        }
        goto tail;
```

**flash_update_entry @6f48** (reached from case-0 golden when bit17/minor>0x18):

```c
    w20 = [self+0x16C];                    ; attempt counter
    name = getName();
    if (w20 < 2) goto third_phase @711c;   ; attempts remaining → do SPI work
    IOLog(fmt@0x1656 "giving up on flash update. max attempts = %u", ..., [self+0x16C]);
    [self+0x169] = 0;
    if ([self+0x16A]) { ...acknowledgeSetPowerState... }   ; same as give_up
    goto tail;
```

**third_phase @711c — the actual SPI flash update:**

```c
        w8 = [[self+0x138]+0xC];           ; fuse-user word
        IOLog(fmt@0x16A8 "fuse user: 0x%08x", ..., w8);
        w20 = (w8 >> 16) & 0xFF;           ; reconfig-state byte (bits[23:16])
        if (w20 >= 2) {                    ; already updated / in progress
            IOLog(fmt@0x16DE "unknown version %u. giving up on flash update", ..., w20);
            goto tail;
        }

        // --- lazy bitfile load: SPI buffer objects not present yet? ---
        if ((w8 & 3) == 0 && [self+0x1198] == NULL) {   ; primary SPI buffer object missing
            zero buf[0x80];
            snprintf(buf, 0x80, (w8&3)==3 ? fmt@0x1731 : fmt@0x1754, 2, w8==3?0x3E:0x3F, w20);
                                            ; "live_2.6X_0x01_primary[_ec%02x].bin"
            loadBitfile(self, buf, channel=1);          ; bl 0x8628 (OSKextRequestResource)
            goto tail;
        }
        if ([self+0x11A0] == NULL) {       ; secondary SPI buffer object missing → same with
                                            ;   fmt@0x1770/0x1795, channel=2 ("..._secondary...")
        }

        [self+0x16C]++;                    ; increment attempt counter (max 2)
        IOLog(fmt@0x17B3 "changing pointer to golden...", ...);
        change_pointer(self, w1=0);        ; bl 0x8580 — program BOTH SPI channels with static
                                            ;   descriptor tables (see §18.7)
        IOSleep(1000ms);                   ; PLT 0xe89c
        spierase(self, size=0x1400000);    ; bl 0x809c — erase 20MB ("this can take 1-2 minutes")

        // --- program channel 0 (primary) with the loaded image ---
        src = [objA_vt+0x308](objA=[self+0x1198]);   ; "get source" vcall (PAC tag 0xf4f4)
        dst = [objA_vt+0x300](objA);                  ; "get dest" vcall (PAC tag 0x98e1)
        spiprogram(self, channel=0, size=0x1400000, src, dst, w5=1);   ; bl 0x821c

        // --- program channel 1 (secondary) — identical with [self+0x11A0] ---
        spiprogram(self, channel=1, size=0x1400000, srcB, dstB, w5=1);

        IOLog(fmt@0x17F5 "reconfiguring to live...", ...);
        [self+0x170] = 1;                  ; arm ext-config blob flush
        x0 = [self+0x1178];                ; commit object D
        PAC-verify(x0, tag 0xcda1);
        [obj_vt+0x1F0](obj, w1=0x3E8);     ; commit vcall (timeout 1000)
        [BAR0+0x2004] = 0x0001A000;        ; engine re-enable word — FLASH-UPDATE variant
                                            ;   (0x9000 + 0x1000, vs 0x9000 for pointer restore)
        goto tail;
```

**Bitfile naming** (snprintf fmts, abs offsets): `live_%u.%u_0x01_primary_ec%02x.bin` @0x1731,
`live_%u.%u_0x01_primary.bin` @0x1754, `live_%u.%u_0x01_secondary_ec%02x.bin` @0x1770,
`live_%u.%u_0x01_secondary.bin` @0x1795. Args: major=**2**, minor=**0x3E (62)** when
`(fuse_user & 3)==3` else **0x3F (63)**, plus reconfig-state byte w20 for the `_ec%02x` variants.
→ golden target versions are exactly **2.62 / 2.63**, matching the case-2 version gate.

### 18.6 New AB fields discovered by this decode

| offset | type | meaning (evidence) |
|--------|------|--------------------|
| +0x138 | u64 ptr | **Firmware status block** (never written from __text; read in start() ×4, reconfig_handler ×4, pointer_restore_handler ×3). Sub-fields: [+4]=board-revision word (image type = bits[23:16]: 0=golden, 1=HWTE, 2=live, 3=debug), [+8]=version word (major.u16 / minor.u16; golden targets 2.62/2.63), [+0xC]=fuse-user word (bits[23:16]=reconfig state ≥2 ⇒ done; bits[1:0] select bitfile variant) |
| +0x150 | u64 ptr | **SPI controller object** (spierase@809c): [+0x10]=status word (bit5 = busy, polled with 1s sleeps), [+0x40]=op size, [+0x48]=marker (0xEEEE set/cleared around op), [+0x54]/[+0x58]=op flags |
| +0x169 | u32 byte | reconfig sub-flag — cleared on give-up paths |
| +0x16A | u32 byte | **deferred power-ack pending** — set elsewhere; when non-zero at give-up, `IOService::acknowledgeSetPowerState()` is called and it's cleared (log "deferred ack for power state change") |
| +0x16C | u32 | **flash-update attempt counter** — incremented in third_phase @7168; ≥2 ⇒ skip SPI work / give up ("max attempts = %u") |
| +0x170 | u32 byte | **ext-config blob pending flag** — set before every commit vcall; consumed at next entry (4KB flush to PCI ext config) |
| +0x174 | 4KB buffer | reconfig blob written to PCI extended config space via `IOPCIDevice::extendedConfigWrite32` ×1024. **No __text writer found** — presumably firmware DMA-fills it during the commit vcall (open item) |
| +0x1178 | u64 ptr | **commit object D** — vt+0x1F0(timeout=1000) "commit reconfig" vcall; used by live-flash-update path and third_phase |
| +0x1188 | u64 ptr | **commit object E** — same vt+0x1F0 contract; used by pointer-restore ("update complete") path |
| +0x1198 | u64 ptr | **primary SPI buffer object** — vt+0x308="get source", vt+0x300="get dest" (PAC tags 0xf4f4/0x98e1); NULL ⇒ lazy loadBitfile(channel=1) |
| +0x11A0 | u64 ptr | **secondary SPI buffer object** — same vtable; NULL ⇒ lazy loadBitfile(channel=2) |

All of these are set up outside __text (init-time object allocation / IOKit registration),
consistent with every other AB object pointer.

### 18.7 Helper functions decoded this session

```c
// @0x8580 — "change_pointer(uint32)" per §16 table: programs BOTH SPI channels
void AB::change_pointer(u32 flag) {
    spierase(self, size=0x13FF000);              ; ~32MB erase prep (w = 0x13F_FF40 - 0xF40) — verified §20.5
    const u8* descA = (flag==1) ? abs0x5F0 : abs0x670;   ; static SPI command descriptor tables
    const u8* descB = (flag==1) ? abs0x630 : abs0x6B0;   ; (binary data, not strings)
    spiprogram(self, ch=0, size=0x13F_FF40, src=descA, w4=0x40, w5=0);
    spiprogram(self, ch=1, size=0x13F_FF40, src=descB, w4=0x40, w5=0);   ; tail call
}

// @0x809c — "spierase(uint32)" per §16: start erase on [self+0x150], wait for completion
void AB::spierase(u32 size) {
    IOLog(fmt@abs0x1389 "begin erasing flash", ...);
    IOLog(fmt@abs0x13CA "erasing 0x%08x... (this can take 1-2 minutes)", ..., size);
    obj = [self+0x150];
    [obj+0x54]=1; [obj+0x58]=0; [obj+0x40]=size; [obj+0x48]=0xEEEE;
    while ([obj+0x10] & (1<<5)) IOSleep(1000ms);   ; busy-poll status bit5, 1s granularity
    [obj+0x4C]=0; [obj+0x54]=1→0 bookkeeping;      ; clear marker/flags on completion
}

// @0x8628 — "loadBitfile(const char*, uint32)" per §16: fetch bitfile from kext bundle
void* AB::loadBitfile(const char* name, u32 channel) {
    IOLog(fmt@abs0x1BE6 "requesting resource data from %s", ..., name);
    void* res = fn@0xe7ec();                       ; getter → &global+0x10 (kext object ptr)
    int r = OSKextRequestResource(res, name, &pac_data_ptr(abs0x87C8), self?, &out);  ; PLT 0xe8a8
    if (!r) { IOLog(fmt@abs0x1C37 "OSKextRequestResource failed: %d", ..., r); return NULL; }
    return out;                                    ; loaded bitfile data (→ SPI buffer objects)
}

// @0x74c4 — "reconfig_timer(IOTimerEventSource*)" per §16: tiny leaf, no stack frame
void AB::reconfig_timer(IOTimerEventSource* src) {
    obj = [self+0x1180];                           ; reconfig-status object (PAC tag 0xcda1)
    [obj_vt+0x1F8](obj, 0, 0, 0);                  ; "poll/notify" vcall — all-zero args
}
```

**spiprogram@0x821c signature (from both call sites):** `spiprogram(self, channel, size, src, dst_or_cmd, mode)` —
change_pointer passes static descriptor tables + w4=0x40/w5=0; reconfig third_phase passes
vt+0x308/vt+0x300 results (src/dst) + w5=1. Exact x4/w5 semantics: open item.

### 18.8 Linux driver implications

- The whole flash-update path is **one-shot board provisioning** (live→golden, max 2 attempts,
  1–2 min erase). A fast decode driver does NOT need it — but the **BAR0[+0x2004] re-enable
  words** (0x00019000 pointer-restore / 0x0001A000 flash-update) and the **PCI ext-config 4KB
  blob flush** are the only parts that could matter for recovery after a firmware state change.
- `reconfig_timer`'s `[self+0x1180] vt+0x1F8(0,0,0)` is the periodic reconfig-status poll —
  if we ever implement live→golden migration on Linux, this is the completion-poll contract.
- The **fuse-user word** ([AB+0x138]+0xC) encodes board provisioning state (bits[23:16] ≥ 2 ⇒
  golden done; bits[1:0] = bitfile variant selector). Read it at probe to decide whether the
  card is fully provisioned.

### 18.9 Open items from this decode

- Who fills **AB+0x174** (the 4KB ext-config blob)? No __text writer — presumed firmware DMA
  during the vt+0x1F0 commit vcall.
- Class/contract of **commit objects D/E** ([self+0x1178]/[self+0x1188]) and their vt+0x1F0
  ("commit", timeout arg) — what does a non-zero return mean?
- **vt+0x1F8** on [self+0x1180] (reconfig_timer poll) — status word layout.
- spiprogram x4/w5 argument semantics across the two call styles.
- fn@0xe7ec global (+0x10 of page 0x10000 object) — which kext/resource handle it is.

## 19. UC lifecycle overrides — full decode (2026-11-06) ✅ (`tmp/dis_uc_lifecycle.txt`)

All remaining ⬜/🟡 local overrides from §2 are now decoded. **None touch HW
directly** — every state change goes through an AB helper (§6). The UC object is
pure bookkeeping: slot table + sContext + space counter.

### 19.1 Destructors / metaclass / allocator

| fn | addr | behavior |
|---|---|---|
| `D1Ev` (complete-object dtor) | 0xcbe0 | Pure tail-call to `IOUserClient::D2`. **No local cleanup** — all teardown lives in stop() |
| `D0Ev` (deleting dtor) | 0xcbe4 | Calls base D2, then `operator delete(this, 0x1970)` via OSObject::dl. **UC instance size = 0x1970 bytes** |
| `getMetaClass()` | 0xcc14 | Returns &0x101C0 (the UC OSMetaClass object; zero-init in binary, built at load) |
| gMetaClass ctor | 0xcc20 | `OSMetaClass::C2("AppleAfterburnerUserClient"@0x309F, super=*GOT[18], size=0x1970)`; vptr = PAC(UC_META vt @0x15578 + 0x10) |
| instance allocator | 0xcc68 | `OSObject::nw(0x1970)` → `IOUserClient::C2(this, &gMetaClass@0x101C0)` → vptr=PAC(UC_vt+0x10) → `instanceConstructed(&gMetaClass)` (mirrors AB allocator @0x4140) |
| in-place ctor variants | 0xcccc / 0xcd24 | Same C2 sequence on a pre-allocated x0, no allocation (OSObject::new(buffer) path) |

### 19.2 start() — provider capture only

```c
bool UC::start(IOService* provider) {          // @0xd1e4
    AB = OSMetaClassBase::safeMetaCast(provider, *GOT[57]);   // GOT[57] = &AB gMetaClass data ptr
    [this+0xD8] = AB;                          // UC+0xD8 (§5)
    if (!AB) return false;
    return IOUserClient::start(provider);      // tail-call base vt slot 192, PAC 0x3c68
}
```

No HW work. (GOT[28] = base IOUserClient vtable — see §2 note.)

### 19.3 stop() — full teardown (the function the Linux driver must mirror)

```c
void UC::stop(IOService* provider) {           // @0xd25c
    AB = [this+0xD8];                          // no NULL check (start() failure ⇒ IOKit won't call stop)
    AB::cleanUp(AB, &UC+0x18F8);               // §15.1 — queue finalization + engine wait(timeout=0)

    for (i = 0; i < 192; i++) {                // slot table @UC+0xF0, stride 0x18
        desc = [slot_i.desc];
        if (!desc) continue;
        AB::unbindFrame(AB, desc);             // space pool: used -= len (saturating)
        len  = [desc_vt+0x1F0](desc);          // getLength
        UC_counter@UC+0x1968 -= len;           // reserved-space counter (§5)
        [desc_vt+0x220](desc, 0);              // teardown/detach
        [desc_vt+0x28](desc);                  // release (refcount--)
        slot_i.desc = slot_i.buf_offset@+0x8 = slot_i.size@+0x10 = 0;   // zero released slot fields
    }

    if ([UC+0x1968] > 0) AB::freeSpace(AB, &UC+0x1968);   // release leftover reservation (saturating used -= counter)

    IOLockFree([UC+0x1938]);                   // sContext qA[0] lock (ctx+0x40)
    IOLockFree([UC+0x1948]);                   // sContext qB[0] lock (ctx+0x50)
    IOLockFree([UC+0x1940]);                   // sContext qA[1] lock (ctx+0x48)
    IOLockFree([UC+0x1950]);                   // sContext qB[1] lock (ctx+0x58)
    IOLockFree([UC+0x1960]);                   // trampoline IOLock (§5)

    IOUserClient::stop(provider);              // tail-call base vt slot 193, PAC 0xb085
}
```

New facts:
- **sContext's four queue locks** sit at UC+0x1938/0x1940/0x1948/0x1950 = ctx+0x40/+0x48/+0x50/+0x58 (ctx = &UC+0x18F8) — allocated in initWithTask, freed here.
- stop() does **not** reset the slot-count high-water mark [UC+0x18F0] (only unbindAll/initWithTask do; §5). Irrelevant in practice: the UC object is destroyed after stop().
- Loop recomputes `slot_base = UC + i·0x18 − 0x1800` each iteration (byte-verified @0xd298–0xd378) — no stale-pointer bug.

### 19.4 clientClose / clientDied

```c
u32 UC::clientClose(void) {                    // @0xd3f8
    u32 ret = 0;
    AB = [this+0xD8];
    if (!AB) { IOLog("...returning kIOReturnNotAttached.\n"); ret = 0xE000_02D9; }
    r = IOService::handleClose(this, 0);       // vcall slot 198 (KSYM undef[127]) — base close hook
    if (!(r & 1)) IOLog("...terminate() failed.\n");   // logged when bit0 of return is clear
    return ret;                                // kIOReturnSuccess(0) or 0xE000_02D9
}

void UC::clientDied(void) {                    // @0xd534 — pure tail-call to base slot 295 (PAC 0xea14), boilerplate
```

No slot release in clientClose — teardown happens only in stop(). Error **0xE000_02D9** = "not attached" variant of the kIOReturnNotAttached family (§8: base 0xE000_02C2, no-space 0x2D8).

### 19.5 willTerminate / didTerminate / terminate / finalize

```c
bool UC::willTerminate(IOService** provider, u32 options) {   // @0xd54c — slot 134
    IOLog("%s[%p]::%s(%p, %u)\n", this, "willTerminate", getName(), provider, options);
    return IOUserClient::willTerminate(provider, options);    // tail-call base vt slot 134, PAC 0xa51b
}

void UC::didTerminate(IOService* provider, u32 options, bool* relinq) {   // @0xd60c — slot 135
    IOLog("%s[%p]::%s(%p, %u, %p)\n", this, "didTerminate", getName(), provider, options, relinq);
    *relinq = false;                              // strb wzr — do NOT relinquish secondary claim (single-client model)
    IOUserClient::didTerminate(provider, options, relinq);   // tail-call base vt slot 135, PAC 0x4577
}

void UC::terminate(u32 options) {                 // @0xd6d8 — slot 200: pure tail-call to base (PAC 0x5ec5), no local work
void UC::finalize(u32 options) {                  // @0xd6f0 — slot 201: pure tail-call to base (PAC 0x5219), no local work
```

### 19.6 Linux driver implications

- **On client disconnect** (fd close / process death): mirror stop() exactly — cleanUp(sContext) → release all bound slots (unbindFrame + counter decrement + zero slot fields) → freeSpace(leftover counter) → destroy the four queue locks + trampoline lock.
- `didTerminate` sets *relinquishSecondary=false ⇒ **single-client model**: never hand the device to another client on termination.
- No lifecycle override writes HW directly; all BAR1 space-pool state changes go through AB::cleanUp/unbindFrame/freeSpace (§6).

## 20. pointer_restore_handler @0x7508 + setPowerState @0x7C60 — full decode (batch 2, 2026-11-06) ✅

### 20.1 Identity & registration
| fn | Role | Registration |
|---|---|---|
| @0x7508–0x7C1B | `pointer_restore_handler(IOInterruptEventSource*, int)` — PCI ext-config restore + image-type state machine (golden/HWTE/live/debug) | PAC-authenticated callback; registered in start() via PLT[0xe950]=IOInterruptEventSource::interruptEventSource. Not a vtable method, no direct bl callers (§16 entry 45) |
| @0x7C1C–0x7C5F | `pointer_restore_timer(IOTimerEventSource*)` — tail-calls `[self+0x1190]->vt[63](obj, 0, 0, 0)` (PAC tag 0xcda1, key 0xa28c) | timer callback; new field AB+0x1190 (§20.4). §16 entry 44 |
| @0x7C60–0x821B | `setPowerState(u32 state, IOService*)` — decoder enable/disable + deferred power-ack + commit vcall | AB vtable slot 259 (§16 row 13) |

### 20.2 pointer_restore_handler @0x7508 — full body (byte-exact)
```c
void AB::pointer_restore_handler(IOInterruptEventSource* src, int type) {   // x0=self, PAC-authenticated entry
    // Phase 1: PCI ext-config flush (identical to reconfig_handler §18.2)
    if ([self+0x170]) {                                   ; ext-config pending flag
        for (i = 0; i < 0x400; i += 4)                    ; 256 dwords = 4KB
            extendedConfigWrite32([self+0x88], i, [self+0x174+i]);   // PLT[0xe8cc]=imports[13] (write stub @0xe8cc; read stub @0xe8c0 called only from start() @0x52cc)
        [self+0x170] = 0;
    }
    // Phase 2: firmware status block ([self+0x138]; layout §18.6)
    w = [[self+0x138]+4];                                 ; "board revision" word — image-type enum in bits[19:16]
    IOLog(fmt@abs0x979  "board revision: 0x%08x", ...);   ; PAGE-0 string (adrp #0)
    v = [[self+0x138]+8];                                 ; image revision word
    IOLog(fmt@abs0x9B4  "image revision: 0x%08x (%u.%u)", ..., v, v>>16, v&0xFFFF);   ; PAGE-0 string
    // Phase 3: switch ((w >> 16) & 0xF) — jump table @0x7C0C (4×s32 data words placed at fn tail inside __text after retab; base PC = 0x7648 via ADR):
    //          [0]=+0xC→golden@0x7654, [1]=+0x190→HWTE@0x77D8, [2]=+0x1D8→live@0x7820, [3]=+0x1F0→debug@0x7838; index >3 → default @0x7790
}
```

**case 0 (GOLDEN) @0x7654–0x778C:**
1. IOLog "image type is golden" (fmt@abs0x161D, page-0x1000+0x61D)
2. IOLog "changing pointer to live..." (fmt@abs0x1A4C)
3. `change_pointer(self, flag=1)` → bl 0x8580: programs the **LIVE** pointer table into flash (§20.5)
4. IOLog "reconfiguring to live..." (fmt@abs0x17F5)
5. `[self+0x170] = 1` — arms ext-config flush for next entry
6. Commit vcall: `[[self+0x1188]->vt+0x1F0](obj, w1=0x3E8)` — **commit object E** (same as reconfig pointer-restore path §18.5), timeout 1000ms, PAC key 0x246e
7. `[BAR0+0x2004] = 0x0001A000` — engine re-enable word, **flash-update variant** (mov w9,#0xa000; movk #1,lsl#16 → str [self+0x148]+4)
8. `b #0x7880` → bare epilogue (retab @0x7898). **No deferred-ack check on this path.**

**case 1 (HWTE) @0x77D8–0x781B:** getName + save args, x0 = fmt@abs0x1832 "image type is HWTE..." → `b #0x787c` (shared IOLog call + full epilogue).
**case 3 (DEBUG) @0x7838–0x789B:** physically interleaved between the two halves of case-2's version gate; getName, x0 = fmt@abs0x19BB "image type is debug..." → falls through to the same shared IOLog @0x787c + epilogue.
**default @0x7790–0x77D4:** IOLog **Error** fmt@abs0x19F6 "image type is unknown..." → `b #0x787c`.

**case 2 (LIVE / flash complete) @0x7820 — decoder bring-up:**
```c
// version gate: w24 = major, w23 = minor (from [[self+0x138]+8])
if (major==2 && minor==0x3E) variant = 1;                 ; fast path @0x7820–0x7834 (2.62)
else { cset/cset/and: variant = (major==2) & (minor==0x3F); }   ; slow path @0x789C–0x78AC (2.63)
getName();                                                ; shared @0x78B0
if (!variant) goto give_up;                               ; cbz w20,#0x7ac8 @0x78E4 — neither 2.62 nor 2.63
IOLog("flash update is complete!");                       ; fmt@abs0x1A8C
[self+0x158] = NULL;                                      ; clear decoder pointer before walk
dir = [self+0x128];  i = 0;                               ; BAR0 directory base (§13); i = BYTE offset
loop:                                                     ; @0x7918
    entry = dir[i];                                       ; ldr w26,[x9, w8, uxtw]
    IOLog Info("Section header: 0x%08x", entry);          ; fmt@abs0x1ACA — logs EVERY section visited
    if ((entry & 0xFF) == 1) { [self+0x11D0] = 1;         ; decoder found (walk CONTINUES to terminator!)
                              [self+0x158] = dir + i; }   ; pointer to the section header word
    i += (entry >> 14) & 0x3FFFC;                         ; stride in BYTES
    if ((entry & 0xFF) != 0) goto loop;                   ; type 0 = terminator
if ([self+0x158]) {                                       ; decoder found:
    IOLog("Decoder section header: 0x%08x", [sec+0]);     ; fmt@abs0x1B05 (readback of hdr word 0)
    if (sec) [sec+4] = 0;                                 ; DISABLE
    IOLog("Decoder Control Register: 0x%08x (disabling)", readback);   ; fmt@abs0x1B48
    IOSleep(1);                                           ; bl PLT[0xe89c]=_IOSleep, w0=1 — mandatory 1s settle delay!
    if (sec) [sec+4] = 1;                                 ; ENABLE
    IOLog("Decoder Control Register: 0x%08x (enabling)", readback);   ; fmt@abs0x11DF
    configureInterrupts(self);                            ; bl 0x5A3C — INIT WRITER (§20.6)
} else {
    IOLog Error("Could not find decoder register section!");          ; fmt@abs0x1B99 (no trailing \n in string)
}
goto tail_A;                                              ; @0x7AC4 / fall-through @0x7B34

give_up:                                                  ; @0x7AC8 — version mismatch only
    IOLog Error("giving up on flash update. max attempts = %u", [self+0x16C]);   ; fmt@abs0x1656 (attempt counter)
    [self+0x169] = 0;                                     ; clear reconfig sub-flag
    if ([self+0x16A]) {                                   ; power ack was deferred during reconfig:
        IOLog("deferred ack for power state change");     ; fmt@abs0x1973
        goto tail_B;                                      ; b #0x7BF8 — NO decoder re-disable here
    }
    goto epilogue;                                        ; bare retab

tail_A:                                                   ; @0x7B48 — bring-up success AND no-decoder paths
    [self+0x169] = 0;
    if ([self+0x16A]) {                                   ; deferred ack pending:
        IOLog("deferred ack for power state change");     ; fmt@abs0x1973
        if (sec) [sec+4] = 0;                             ; **re-DISABLE decoder** before acking
        IOLog("Decoder Control Register: 0x%08x (disabling)", readback);   ; fmt@abs0x1B48
    }
tail_B:                                                   ; @0x7BF8 — shared by both deferred-ack blocks
    IOService::acknowledgeSetPowerState(self);            ; bl PLT[0xe9a4 — delivers the deferred ack to IOKit
    [self+0x16A] = 0;                                     ; clear deferred-ack flag
epilogue:                                                 ; @0x7880–0x7898 — register restore + retab (shared by ALL cases)
```

The walk is byte-exact identical to §12.2 (handleInterrupt bitfile section) and matches Phase-3k's live BAR0 walk (terminator at +0x802C on the lab card). The deferred-ack tail exists **only in case-2 paths** — golden/HWTE/debug/default exit directly through the bare epilogue.

### 20.3 setPowerState @0x7C60–0x821B — full body (byte-exact)
```c
IOReturn AB::setPowerState(u32 state, IOService* provider) {   // vtable slot 259; x21 = self+0x11D0 (field-group base reg)
    // NOTE: every IOLog below is preceded by name=[self_vt+0x3A8](self, 0) — getName-style log helper (key 0xc9c), standard kext pattern (§15.2)
    IOLog("requested power state: %lu", state);                 ; fmt@abs0x1090 (Info; funcname string "setPowerState" @abs0x10CF)
    switch (state) {
    case 1:                                                     // POWER ON — enable decoder → common tail
        sec = [self+0x158];
        if (!sec) {                                             ; cbz @0x7d5c
            if ([self+0x169]) { IOLog Error("flash update in progress"); goto tail; }   ; fmt@abs0x1136 — reconfig: skip, no disable
            goto disable_path;
        }
        if ([self+0x169]) {                                     ; cbnz @0x7d68 — decoder present but reconfig in progress:
            IOLog Error("flash update in progress");            ; fmt@abs0x1136 — skip enable (decoder stays as bring-up left it)
            goto tail;
        }
        if ((hdr=[sec+0]) & 0xFF != 1) goto disable_path;       ; b.ne @0x7d74 — not a type-1 section
        [sec+4] = hdr & 0xFF;                                   ; = 1 (enable)
        IOLog("Decoder Control Register: 0x%08x (enabling)", readback);   ; fmt@abs0x11DF
        goto tail;
    case 0:                                                     // POWER OFF — wake gate + commit, EARLY RETURN (no common tail!)
        if ([self+0x169]) {                                     ; reconfig in progress: DEFER the ack
            IOLog Error("flash update in progress. deferring power state ack");   ; fmt@abs0x10DD
            [self+0x16A] = 1;                                   ; arm deferred-ack flag (consumed by §20.2 tail_A/tail_B)
            return 0x11E1A300;                                  ; non-zero IOReturn = "will ack later" (b #0x7FFC epilogue)
        }
        [self+0x12F8] = 1;                                      ; arm WAKE GATE (gates pre-submit Wake call, §15.2) — strb w8,[x21+#0x128] @0x7E24
        obj = [self+0x1300]; PAC-verify(obj);                   ; commit object F — NEW field (§20.4), tag 0xcda1
        [obj->vt+0x1F0](obj, w1=0x3E8);                         ; commit vcall (key 0x246e), timeout 1000ms @0x7E5C–0x7E60
        return 0x4C4B40;                                        ; non-zero IOReturn on power-down path! (mov w0,#0x4b40; movk #0x4c,lsl#16)
    default:
        IOLog Error("Unexpected requested power state %lu", state);   ; fmt@abs0x127F
        return 5;                                               ; @0x7E18 (mov w0,#5)
    }
disable_path:                                                   // @0x7EC0 — decoder lost / not configured
    [self+0x11D0] = 0;                                          ; clear decoder-found flag (strb wzr,[x21])
    [self+0x158] = NULL;                                        ; clear decoder pointer
    IOLog Error("lost decoder configuration. driver support for PR has been deprecated");   ; fmt@abs0x1174
tail:                                                           // common exit (state==1 paths only)
    if ([self+0x160]) {                                         ; thermal-distress bitmask
        IOLog Info("Resetting thermal distress condition 0x%08x", [self+0x160]);   ; fmt@abs0x122F
        [self+0x160] = 0;
    }
    configureInterrupts(self);                                  ; bl 0x5A3C — INIT WRITER, re-applied on every power-ON exit (§20.6)
    [self+0x16A] = 0;                                           ; clear deferred-ack flag
    eng = [self+0x98]; PAC-verify(eng);                         ; second engine object (class still open, §14 item 12)
    [eng->vt+0x180](eng);                                       ; NEW slot — key 0x8bec @0x7FA8; no args beyond self
    if ([self+0x12F8]) {                                        ; wake gate armed by a prior power-OFF:
        x1 = &AB+0x12F8;                                        ; wake-gate ADDRESS passed as ctx arg! (add x1,x21,#0x128)
        [self+0x12F8] = 0;                                      ; clear it @0x7FB8
        eng2 = [self+0x90]; PAC-verify(eng2);                   ; completion-notify engine object (§15.2/§17)
        [eng2->vt+0x208](eng2, &AB+0x12F8, w2=0);               ; key 0xf36c @0x7FF4
    }
    return 0;                                                   ; @0x7FF8 (mov w0,#0)
}
```

**Key semantics:**
- **Deferred power-ack protocol**: setPowerState(0) during reconfig does NOT disable the decoder — it arms [self+0x16A] and returns non-zero (0x11E1A300). pointer_restore_handler's tail_A/tail_B later calls acknowledgeSetPowerState() to deliver the ack. This is why the kext never disables the decoder mid-flash-update.
- **Wake-gate lifecycle**: power-OFF arms [self+0x12F8] + commits via object F; the NEXT power-ON exit consumes it (clears gate, notifies eng2=[self+0x90] via vt+0x208 with &gate as ctx). The pre-submit Wake call (§15.2) is gated on this same byte.
- **Init writer runs on every power-ON exit** — BAR1 +0x2004=0x16 etc. are re-applied after any power transition (explains why the Linux driver must re-run init after FLR/power events).
- Non-zero IOReturns 0x11E1A300 / 0x4C4B40 are not standard kIOReturn values — tagged/encoded returns; in the deferred case IOKit treats non-zero as "handled, ack pending".

### 20.4 New AB fields (add to §13 table)
| Offset | Meaning | Evidence |
|---|---|---|
| +0x1300 | **commit object F** — same vt+0x1F0("commit", timeout=0x3E8) contract as D/E (§18.6); used by setPowerState(0) power-down path; PAC key 0x246e, tag 0xcda1 | fn@0x7C60 @0x7E28–0x7E60 |
| +0x1190 | **pointer_restore_timer target object** — vt+0x1F8(obj, 0, 0, 0), key 0xa28c (same slot offset as dmaQ completion-record cleanUp §15.1) | fn@0x7C1C @0x7C44–0x7C5C |
| +0x98 | **second engine object** — vt+0x180(eng), key 0x8bec, called from setPowerState tail (distinct from completion-notify object at +0x90) | fn@0x7C60 @0x7F84–0x7FA8 |

### 20.5 change_pointer @0x8580 — verified (corrects §18.7)
```asm
w1 = 0x13FFF40 - 0xF40 = **0x13FF000**          ; spierase size (mov/movk/sub @0x8598–0x85A0) — §18.7's "0x13F0000" was a typo
bl  spierase(self, 0x13FF000)                   ; ~20MB erase prep
csel x3,  abs0x5F0, abs0x670, (flag==1)         ; ch0 source table @0x85BC
csel x19, abs0x630, abs0x6B0, (flag==1)         ; ch1 source table @0x85D0
bl  spiprogram(self, ch=0, off=0x13FFF40, src=x3, w4=0x40, w5=0)   ; @0x85EC
b   spiprogram(self, ch=1, off=0x13FFF40, src=x19, w4=0x40, w5=0)  ; tail call @0x8624 (after epilogue PAC check)
```
Both SPI channels get a **64-byte block** programmed at flash offset **0x13FFF40**. The sources are four static **SPI command descriptor tables** in the kext binary (page-0 region, 16 dwords each, sentinel-terminated), verified from `rework/arm64e.bin`:

| Table | Used by | Contents (16×u32 LE) |
|---|---|---|
| @abs0x5F0 | ch0, flag=1 (LIVE) | `a9560000 00000201 01ff0201 20000000 101ccf5 1010000 10003 10009 0 0 0 0 1 0 0 FFFFFFFF` |
| @abs0x670 | ch0, flag=0 (GOLDEN) | `a9560000 00000201 01ff0201 00000000 101ccf5 1010000 10003 10009 0 0 0 0 1 f0000 0 FFFFFFFF` |
| @abs0x630 | ch1, flag=1 (LIVE) | `a9562000 20003020 40ff3000 8002000 3020343e 30c00400 30800001 30800000 20002000×5 20003080 2000 20002000 FFFFFFFF` |
| @abs0x6B0 | ch1, flag=0 (GOLDEN) | `a9562000 20003020 40ff3000 102000 3020343e 30c00400 30800001 30800000 20002000×5 20003080 2000 20002000 FFFFFFFF` |

Family A (ch0, magic `0xa9560000`) differs between LIVE/GOLDEN only at dword[3] (`0x20000000` vs `0`) and dword[13] (`0` vs `0xF0000`); family B (ch1, magic `0xa9562000`) differs only at dword[3] (`0x8002000` vs `0x102000`). These are firmware-side SPI command sequences — the differing dwords select which golden/live entry pointer set gets written. (Earlier draft's "[addr, SENT] pointer list" contents were a misremembering — no such pattern exists anywhere in the binary; corrected here.)

spiprogram itself is at **@0x821C** — its internals (how w4/w5 drive the SPI write) remain open item §14-19. The function immediately following change_pointer, **loadBitfile @0x8628** (§14 item 10), acquires the bitfile resource via `OSKextRequestResource` with a PAC-authenticated IOReg handle (ptr@abs0x87C8, key 0x5e7e) and brackets it with `[self->vt+0x3A8](self, 0)` (key 0xc9c).

### 20.6 fn@0x5A3C = `configureInterrupts` (§16 entry 32) — the "init writer"
Call sites now confirmed in **three** places: reconfig_handler success path (§18), pointer_restore_handler case-2 bring-up (@0x7AC0), setPowerState common tail. Closes the Phase-3k "who calls it" question: re-applied on every power-state transition and after decoder bring-up (BAR1 +0x90/+0x1090=0xFFFFFE, +0x2010=3, +0x2004=0x16, BAR0 magic region).

### 20.7 PLT resolutions (batch 2)
| Stub | Import idx | Symbol | Call sites in __text |
|---|---|---|---|
| 0xe8c0 | 12 | IOPCIDevice::extendedConfigRead32 | start() @0x52cc only |
| 0xe8cc | 13 | IOPCIDevice::extendedConfigWrite32 | reconfig_handler @0x6be4, pointer_restore_handler @0x7548 (ext-config flush loops) |
| 0xe89c | 8 | _IOSleep | pointer_restore_handler case-2 disable/enable settle delay @0x7A58; setPowerState tail region @0x8194 |
| 0xe83c | 0 | _IODelay | flash-write helper (batch 1) |
| 0xea1c | 51 | _snprintf | batch 1 |

### 20.8 Implications for the Linux driver
1. **Decoder bring-up sequence is now fully known** and matches what our mode=4/5 live tests did manually: disable [sec+4]=0 → wait ≥1s (IOSleep(1) — mandatory!) → enable [sec+4]=1 → configureInterrupts init writer. The kext does this on every power-on after a flash update.
2. **Image-type enum** (bits[19:16] of [status+4]): golden=0, HWTE=1, live=2, debug=3 — matches §18's flow: flash update runs from a GOLDEN image; on completion the handler switches pointers to LIVE and re-enables with 0x0001A000.
3. **Commit object F ([self+0x1300])** is a fourth commit-object instance (D/E/F + [self+0x1180] poll target) — all sharing the vt+0x1F0 contract; class still open (§14 items 12/18).
4. **Deferred power-ack** has no Linux analog (no IOKit power management), but it explains why the kext never disables the decoder mid-flash-update and why [self+0x16A] must be checked before any decoder teardown.
5. The give_up path does NOT re-disable the decoder; only tail_A's deferred-ack block does ([sec+4]=0 + "(disabling)" log) — mirror this asymmetry if we ever implement flash-update recovery on Linux.
