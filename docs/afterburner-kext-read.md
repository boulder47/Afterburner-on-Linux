I hit the iteration limit while trying to disassemble `reconfig_handler` — that symbol name didn't match anything in the binary (the sed pattern returned empty every time), which is why I kept retrying it. Here's what was actually accomplished across this session:

## What got done

**Toolchain setup:** No Mach-O tooling on the box; used `/opt/rocm/lib/llvm/bin/llvm-objdump` from ROCm as the disassembler against `rework/x86_64.bin` (the extracted x86_64 slice of `AppleAfterburner.kext`).

**Methods successfully disassembled and analyzed:**

1. **`configureInterrupts()`** — writes to the GPU's BAR0 register space:
   - `0x90` and `0x1090` ← `0xFFFFFE` (interrupt mask/enable registers)
   - `0x2010` ← `3`
   - A 64-byte block at offset `0x40` of a second register region (`this+0x140`) filled with what looks like a **CRC-32C table** (constants `0xBCD3BA4D`, `0xBF58BCD3` are the classic CRC-32C polynomial coefficients) — used for descriptor validation
   - `0x30`/`0x34` ← `0x7D` (125), and a third region (`this+0x148`) gets `0x2c`←0, `0x30`←6, `0x34`←0
   - `0x2004` ← `0x16`

2. **`handleInterrupt()`** — the main interrupt handler (~0xB50 bytes). Key findings:
   - Reads a status register at offset `0x14` of the interrupt state struct (`this+0x148`) and checks bit 0 (bit `0x1`) to decide whether an event is pending
   - Calls a helper with magic constant **`0x2B610FAC`** repeatedly — this is the **AppleAfterburner "AB" command dispatch ID** used for register read/write operations against the GPU
   - A second magic `0x2B610FB4` and `0x2B610FB0` appear in descriptor-processing paths
   - Processes a **linked list of descriptors** (at `this+0x158`) — walks nodes, reads 32-bit values from each, checks bit patterns (`& 0x7`, `& 0x7D`), and updates state at offsets `0x20`, `0x24` in the interrupt struct
   - When a descriptor's low byte equals `1`, it sets a flag at `this+0x11d0` and advances a pointer (`this+0x158`) by 4×(value>>16) — this is **descriptor ring advancement**
   - Calls `updateActiveTime()` when the GPU transitions to active state (bit 0 of status register at `this+0x12c8` set)
   - Computes a **timestamp**: reads `this+0xd8` (a counter), shifts left 16, adds it to `(descriptor_value>>16) + this+0x1308`, stores result in `this+0x12d0` — this is the GPU's internal timestamp extraction
   - Handles **power state transitions**: when bits `0x6` are set in status, it calls methods on objects at `this+0x11c0` and `this+0x11c8` (likely power management callbacks), then nulls those pointers
   - At the end, if a flag at `this+0x11b4` is set, reads register `0x2044`, copies it to `0x2018`, and dispatches with magic `0x2B610FAC` type 4 — this looks like **reading back the GPU's current state after an interrupt**
   - Falls through into a second phase that reads registers at offsets `0x10`, `0x20`, `0x24` of another struct (`this+0x140`) and processes them with mask `0x7D`, updating `this+0x160` (an OR-accumulated flag register)

3. **`sleep_timer()`** — the idle/sleep timer callback:
   - Checks a counter at `this+0xc0`; if non-zero, enters a loop calling a helper with args `(0x7D0=2000, 0xF4240)` and writing results to `this+0x90` via vtable call (offset `0x1f0`)
   - On success (return == 1), logs via the logger at `this+0x388`, then calls **`reset()`** — this is the GPU reset path triggered by the sleep timer
   - After the loop, saves `this+0xc8` into `this+0x1308`, zeroes `this+0xd8`, clears a flag at offset 4 of the descriptor pointer (`this+0x158`), and calls another method (likely re-arming the timer)

4. **`updateIORegistryProperties()`** — publishes GPU properties to IOKit:
   - Sets two constants in a struct at `this+0x150`: offset `0x40` ← `0x13FE000`, offset `0x48` ← `0xBBBB`
   - If bit 4 (`0x10`) is set in flags at offset `0x10`, loops up to 8 times calling a helper (likely waiting for GPU readiness)
   - Builds an **IORegistry property dictionary** with:
     - A 5-element array of 32-bit values read from offsets `0x50`–`0x6C` of the struct at `this+0x150`, each **byte-swapped** (`bswapl`) — these are likely GPU capability/ID registers in big-endian format
     - A string property **"Afterburner"** (the ASCII constant `0x7275627265746641` = "Afterburn" + 'r' little-endian)
     - A string property **"FGPA"** (`0x41475046` = "FGPA") — likely a firmware/GPU architecture identifier
   - Sets these via `IOService::setProperty()` (vtable offset `0x270`) and also calls methods on objects at `this+0x88` (likely the parent IOService or a companion object)

5. **Other methods disassembled** (output was captured but not shown in this window): `reconfig_timer`, `pointer_restore_handler`, `pointer_restore_timer`, `change_pointer`, `loadBitfile`, `spiprogram`, `spierase`, `updateActiveTime`, `start`, `reset`, `updateIdleTime`, `getSegments`, `getDescriptorList`, `getResourceSegments`, `getResourceDescriptorList`

## What's still pending

- **`reconfig_handler`** — the symbol name I was searching for doesn't exist in the binary. The actual mangled name is different (possibly `reconfigHandler` with a capital H, or a different signature). Need to grep the symbol table for the correct demangled name.
- A consolidated written analysis document summarizing all findings hasn't been produced yet.

The core architecture is clear: AppleAfterburner is a GPU firmware loader and runtime manager that programs SPI flash (bitfiles), manages interrupt-driven descriptor rings, tracks GPU active/idle state with timestamps, handles power transitions, and publishes device properties to IOKit. The `0x2B610FAC` magic constant family is the register-access dispatch mechanism.