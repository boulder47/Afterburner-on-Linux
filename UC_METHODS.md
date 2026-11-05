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
| 2   | +0x10  | 0xcbe0 | `~AppleAfterburnerUserClient() [D1Ev]`            | ⬜ |
| 3   | +0x18  | 0xcbe4 | `~AppleAfterburnerUserClient() [D0Ev]` (deleting) | ⬜ |
| 9   | +0x48  | 0xcc14 | `getMetaClass()`                                   | ⬜ |
| 134 | +0x430 | 0xd54c | `willTerminate(IOService*, u32)`                   | ⬜ |
| 135 | +0x438 | 0xd60c | `didTerminate(IOService*, u32, bool*)`             | ⬜ |
| 192 | +0x600 | 0xd1e4 | `start(IOService*)`                                | 🟡 |
| 193 | +0x608 | 0xd25c | `stop(IOService*)`                                 | ✅ (slot-release loop; writes UC+0x1968 @0xd2f0) |
| 200 | +0x640 | 0xd6d8 | `terminate(u32)`                                   | ⬜ |
| 201 | +0x648 | 0xd6f0 | `finalize(u32)`                                    | ⬜ |
| 274 | +0x890 | 0xcfc8 | `externalMethod(...)` — dispatch tail              | ✅ (`tmp/dis_uc_externalMethod.txt`) |
| 292 | +0x920 | 0xd004 | `initWithTask(task*, void*, u32, OSDictionary*)`   | ✅ (`tmp/dis_uc_initWithTask.txt`) |
| 294 | +0x930 | 0xd3f8 | `clientClose()`                                    | ⬜ |
| 295 | +0x938 | 0xd534 | `clientDied()`                                     | ⬜ |
| 308 | +0x990 | 0xd708 | `submitFrame(s_inputDataStruct*, u64*)`            | ✅ §4.1 |
| 309 | +0x998 | 0xdcc4 | `queryCompletion(u64, u64*)`                       | ✅ §4.2 |
| 310 | +0x9a0 | 0xdd9c | `bindFrame(s_bindDataStruct*, u64*)`               | ✅ §4.3 |
| 311 | +0x9a8 | 0xe2a4 | `cardStatus(u64*)`                                 | ✅ §4.4 |
| 312 | +0x9b0 | 0xe36c | `checkForSpace(s_bufDataStruct*)`                  | ✅ §4.5 |
| 313 | +0x9b8 | 0xe4c0 | `unbindAll()`                                      | ✅ §4.6 |
| 314 | +0x9c0 | 0xe6a4 | `getPercent(u64*)`                                 | ✅ §4.7 |

Slots 306/307 = inherited IOKit virtuals (KSYM tag 0xC0). Slot 119 (+0x3a8) =
IORegistryEntry::getName — used by every error-log path as `getName(UC, 0)`.

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
| cleanUp | 0xb0ec | `(sContext*)` | card-side state reset (called first by unbindAll) | 🟡 body TODO |
| **reset** | **0xaea8–0xb0ec** | `()` | **per-frame teardown**: clear both doorbells, wait for c2h/h2c status to drain, free 2 desc queues, fw handshake, reset all completion tracking. See §10.3 — CRITICAL for Linux driver | ✅ (`tmp/dis_ab_reset.txt`) |
| queryCompletion | 0xb88c–0xbd90 | `(sContext*, u64 frameID, u64* out)` | **NEVER writes *out** (x3 clobbered @0xb908; no store to it anywhere). Waits on engine via PAC'd stub, then drains 4 DMA completion queues. See §11 | ✅ (`tmp/dis_ab_queryCompletion.txt`) |
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

kernel_debug trace id: **0x2B61FA4** (class arg 3/4/5 at various points).

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

## 12. AB::handleInterrupt 🟡 (body 0x5aec–0x6a00, `tmp/dis_ab_handleInterrupt.txt`)

IRQ entry point (`(OSObject*, IOInterruptEventSource*, int)`). Decoded so far — the
completion-event path around 0x64c8:
```
kernel_debug(...); tbz w20,#0 → (other event type)
[AB_vt+0x3a8](AB, 0)                    ; getName-style vcall (vt slot 151, tag 0xc9c)
IOLog(str@0x2F8D)                       ; interrupt log line
updateActiveTime()                      ; ← idle→active transition on IRQ!
[obj+0x114] = 1                         ; byte flag on some channel object (x24)
[AB+0x12DC] = w8                        ; status u32 (from [AB+0x158]->obj ? +0x10 : 0)
[AB+0x12D0] = [AB+0x1308] + ((w8>>16)) + ([AB+0xD8]<<16)   ; computed stamp/mask
[AB+0x12D8] = 0                         ; _numFrames cleared
```
TODO: full body (event-type dispatch, BAR1 status reads, which register bits gate the
updateActiveTime path).

## 13. AB object layout (offsets from `this`) — accumulated ✅/🟡

| offset | field | notes |
|---|---|---|
| +0x90   | engine object ptr | PAC-verified; vt+0x1E8 = wait-with-callback, vt+0x210 = poll-until-deadline |
| +0xA0 / +0xA8 | notifier objects (dmaQ / descDmaQ) | vt+0x150(notifier, obj) on completion |
| +0xC0   | status word (u32) | engine-maintained; returned by cardStatus(); &passed to engine poll; cleared by reset() |
| +0xC8   | second status/counter word | used in done-verify `([C8] & ~[12D0]) == [12D8]`; saved to 0x1308 by reset() |
| +0xD8   | IRQ-stamp high component (u64) | `[AB+0xD8]<<16` in handleInterrupt stamp calc; cleared by reset() |
| +0xE0 / +0xE8 | acc_E0 (active) / acc_E8 (idle) time accumulators | §11 |
| +0xF8   | last-sample timeval | §11 |
| +0x108  | scratch timeval (microtime target) | §11 |
| +0x11F0 / +0x1200 | desc-queue heads (72B nodes, self-ref sentinel) | freed by reset(); per-frame descriptor allocs |
| +0x12C0 | IOLock* — space pool lock | §6 |
| +0x12D0 | stamp/mask for done-verify | written by IRQ handler & stub first-timeout; = -1 after reset() |
| +0x12D8 | _numFrames (u32) | failure counter; cleared on IRQ / first timeout / reset() |
| +0x12DC | status u32 | written by handleInterrupt @0x64e0 |
| +0x12E0 | idle-flag byte | §11 |
| +0x12E8 / +0x12F0 | space pool used / cap (u64) | §6 |
| +0x1308 | IRQ-stamp base = saved [C8] at reset() time | handleInterrupt: `[12D0] = [1308] + ((w8>>16)) + ([D8]<<16)` |
| +0x158  | **decoder control object** | +4 = reset-signal (write 1), +8 = decoder status (bit0=ready/ack; logged "Decoder status"), +0x14 = volatile status read at reset start |

## 14. Open items / next steps

1. ✅ DONE this session: queryCompletion@0xb88c (§10), cardStatus@0xbd90, getPercent@0xbda0, unbindFrame@0xbf6c, bindFrame(desc)@0xbec, updateActiveTime@0xbe64, updateIdleTime@0x9be0 — all in §6
2. 🟡 handleInterrupt full body (event-type dispatch + BAR1 status reads; completion path around 0x64c8 decoded, see §12)
3. ✅ DONE: AB::reset @0xaea8 (§10.3) — per-frame teardown sequence fully decoded
4. 🟡 bindFrame slot-selection logic (dd9c..de8c): auto-assign vs explicit index via in[+0x18]; 0xFF cap meaning
5. 🟡 sContext layout at UC+0x18F8 (fields consumed by AB::submitFrame/cleanUp)
6. 🟡 `[desc_vt+0x1F8](desc, 1)` semantics on submit success
7. 🟡 IODirection enum mapping for withAddressRange dir=1/2
8. ⬜ lifecycle overrides: start@0xd1e4 (🟡), willTerminate/didTerminate/terminate/finalize/clientClose/clientDied/getMetaClass/D1Ev/D0Ev — mostly boilerplate, decode any that touch HW/state
9. 🟡 UC+0x1900 field semantics; s_inputDataStruct +0x14/+0x18 fields
