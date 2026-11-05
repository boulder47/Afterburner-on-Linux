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

Space model: **AB+0x12E8 = used/reserved bytes**, **AB+0x12F0 = cap** (total), lock at **AB+0x12C0**.

| fn | addr | signature | behavior | status |
|---|---|---|---|---|
| checkForSpace | 0xbfec | `(u64* size)` | under lock: if `used > cap` → **0xE000_02EC**; else `used += *size`; return 0 | ✅ (`tmp/dis_ab_checkForSpace.txt`) |
| freeSpace | 0xc058 | `(u64* req)` | under lock: `used = max(0, used - *req)` (saturating); does NOT write back *out; return 0 | ✅ (`tmp/dis_ab_freeSpace.txt`) |
| unbindFrame | 0xbf6c | `(IOMemoryDescriptor*)` | card-side unbind of one bound frame | 🟡 called from unbindAll; body TODO |
| cleanUp | 0xb0ec | `(sContext*)` | card-side state reset (called first by unbindAll) | 🟡 body TODO |
| queryCompletion | 0xb88c | `(sContext*, u64, u64*)` | frame completion query → *out; 0=success | ⬜ body TODO |
| cardStatus | 0xbd90 | `(u64*)` | card status → *out; 0=success | ⬜ body TODO (reads BAR1 status regs per earlier notes) |
| getPercent | 0xbda0 | `(u64*)` | decode progress % → *out; 0=success | ⬜ body TODO |
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
| queryCompletion | **0x2C00** (page 0x2xxx!) | — |
| bindFrame | badarg@0x339A · existing-slot err@0x33E9 · wire-fail@0x3429 · desc-fail@0x344F | name@0x3390 |
| cardStatus | 0x347B | name@0x34A9 |
| checkForSpace | 0x34B4 | name@0x34F2 |
| unbindAll | in-use fmt@0x3500 | name@0x33E9 (shared with bindFrame) |
| getPercent | 0x350A | name@0x353D |
| AB::submitFrame | ab.sf fmt1@0x2638, fmt2@0x2695 | — |

kernel_debug trace id: **0x2B61FA4** (class arg 3/4/5 at various points).

## 10. Open items / next steps

1. ⬜ AB-level bodies the Linux driver must replicate: queryCompletion@0xb88c, cardStatus@0xbd90, getPercent@0xbda0, unbindFrame@0xbf6c, cleanUp@0xb0ec
2. 🟡 bindFrame slot-selection logic (dd9c..de8c): auto-assign vs explicit index via in[+0x18]; 0xFF cap meaning
3. 🟡 sContext layout at UC+0x18F8 (fields consumed by AB::submitFrame/cleanUp)
4. 🟡 `[desc_vt+0x1F8](desc, 1)` semantics on submit success
5. 🟡 IODirection enum mapping for withAddressRange dir=1/2
6. ⬜ lifecycle overrides: start@0xd1e4 (🟡), willTerminate/didTerminate/terminate/finalize/clientClose/clientDied/getMetaClass/D1Ev/D0Ev — mostly boilerplate, decode any that touch HW/state
7. 🟡 UC+0x1900 field semantics; s_inputDataStruct +0x14/+0x18 fields
