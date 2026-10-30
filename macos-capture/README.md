# Apple Afterburner macOS Capture Tools

These tools help capture the macOS-side interaction with the Apple Afterburner card, so we can bring the logs back to the Linux box for analysis.

## Files

- `afterburner_capture.sh` — Main capture script (runs on macOS)
- `afterburner_bar_dump.m` — Objective-C helper to dump PCI BAR registers (builds to a standalone tool)

## Usage

### Step 1: Copy these files to your Mac

```bash
scp afterburner-re/macos-capture/* user@mac:/tmp/afterburner_capture/
```

### Step 2: Build the BAR dump helper (optional but recommended)

```bash
cd /tmp/afterburner_capture
clang -framework IOKit -framework CoreFoundation afterburner_bar_dump.m -o afterburner_bar_dump
chmod +x afterburner_bar_dump
```

### Step 3: Run the capture script

```bash
sudo ./afterburner_capture.sh /tmp/afterburner_capture_output
```

This will create a tarball at `/tmp/afterburner_capture_output.tar.gz` containing:
- `system_info.txt` — macOS version, kernel info
- `ioreg_full.txt` — Full IOKit registry dump
- `ioreg_afterburner.txt` — Afterburner-specific IOKit entries
- `ioreg_dma_commands.txt` — IODMACommand objects (if visible)
- `kernel_log.txt` — Afterburner-related kernel messages
- `bar_registers.txt` — PCI BAR register dump (if helper tool available)
- `prores_processes.txt` — Active ProRes/Afterburner processes
- `kext_status.txt` — Kext load status and version

### Step 4: Bring the tarball back to Linux

```bash
scp user@mac:/tmp/afterburner_capture_output.tar.gz ./
tar -xzf afterburner_capture_output.tar.gz
```

## What we're looking for

The key files to examine are:

1. **`ioreg_afterburner.txt`** — Shows the IOKit object tree for the Afterburner device, including any DMA command objects and their properties. This should reveal how the descriptor ring is configured.

2. **`bar_registers.txt`** — If we can build the BAR dump helper, this will show us the actual register values on macOS (including the desc-ring PA at BAR1+0x3080 and any other registers that are set during init).

3. **`kernel_log.txt`** — May contain debug messages from the kext about what it's doing during init and decode.

## Notes

- The `afterburner_bar_dump.m` tool uses IOKit's `IOServiceGetReg()` to map the PCI BARs. This requires root privileges (`sudo`).
- If the BAR dump helper fails, the `ioreg` output should still contain enough information to understand the descriptor ring layout.
- Run the capture script **while a ProRes decode is active** (if possible) to catch any dynamic register changes or DMA activity.
