#!/bin/bash
# afterburner_capture.sh — Capture macOS-side Afterburner card interactions for Linux driver development.
#
# This script runs on a Mac with the Apple Afterburner kext loaded and captures:
#   1. IOKit registry state (device properties, DMA command objects)
#   2. Kernel log messages from the kext (ioreg + dmesg equivalent)
#   3. PCI BAR register reads (via a small helper tool or ioreg)
#   4. Any ProRes decode activity (if available via system tools)
#
# Usage:
#   sudo ./afterburner_capture.sh [output_dir]
#
# The output will be a tarball you can bring back to the Linux box for analysis.

set -euo pipefail

OUTPUT_DIR="${1:-./afterburner_capture_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUTPUT_DIR"

echo "=== Apple Afterburner Capture Script ==="
echo "Output directory: $OUTPUT_DIR"
echo ""

# ---------------------------------------------------------------------------
# 1. Basic system info
# ---------------------------------------------------------------------------
echo "[1/8] Capturing system info..."
{
    echo "=== System Info ==="
    sw_vers
    echo ""
    echo "=== Kernel Version ==="
    uname -a
    echo ""
    echo "=== PCI Device Info (lspci equivalent) ==="
    system_profiler SPUSBDataType 2>/dev/null | grep -A 10 -i "afterburner\|106b:0205" || \
        echo "(system_profiler did not find Afterburner — checking ioreg)"
} > "$OUTPUT_DIR/system_info.txt" 2>&1

# ---------------------------------------------------------------------------
# 2. IOKit registry dump (the most important part)
# ---------------------------------------------------------------------------
echo "[2/8] Dumping IOKit registry..."
{
    echo "=== Full IOKit Registry (ioreg -l) ==="
    ioreg -l > "$OUTPUT_DIR/ioreg_full.txt" 2>&1
    
    echo ""
    echo "=== Afterburner-specific IOKit entries ==="
    # Find the AppleAfterburner device in the registry
    ioreg -l | grep -B 5 -A 50 "AppleAfterburner\|106b:0205" > "$OUTPUT_DIR/ioreg_afterburner.txt" 2>&1 || \
        echo "(No AppleAfterburner entries found in ioreg)"
    
    echo ""
    echo "=== IODMACommand objects (if visible) ==="
    ioreg -l | grep -B 5 -A 30 "IODMACommand\|DMACommand" > "$OUTPUT_DIR/ioreg_dma_commands.txt" 2>&1 || \
        echo "(No IODMACommand entries found)"
}

# ---------------------------------------------------------------------------
# 3. Kernel log messages (dmesg equivalent)
# ---------------------------------------------------------------------------
echo "[3/8] Capturing kernel logs..."
{
    echo "=== Recent kernel messages (last 500 lines) ==="
    # On macOS, use `log show` or check /var/log/system.log
    if command -v log &> /dev/null; then
        log show --predicate 'process == "kernel"' --last 1h 2>/dev/null | \
            grep -i "afterburner\|106b:0205\|prores" > "$OUTPUT_DIR/kernel_log.txt" || \
            echo "(No Afterburner-related kernel messages found)"
    else
        # Fallback: check system.log
        tail -n 500 /var/log/system.log 2>/dev/null | \
            grep -i "afterburner\|106b:0205\|prores" > "$OUTPUT_DIR/kernel_log.txt" || \
            echo "(Could not access system logs)"
    fi
    
    echo ""
    echo "=== Full kernel log (last 1000 lines, unfiltered) ==="
    if command -v log &> /dev/null; then
        log show --predicate 'process == "kernel"' --last 30m 2>/dev/null | \
            tail -n 1000 > "$OUTPUT_DIR/kernel_log_full.txt" || true
    fi
}

# ---------------------------------------------------------------------------
# 4. PCI BAR register reads (if we have a helper tool)
# ---------------------------------------------------------------------------
echo "[4/8] Attempting PCI BAR register reads..."
{
    echo "=== PCI BAR Register Dump ==="
    
    # Check if we have a custom tool for reading BARs
    if command -v afterburner_bar_dump &> /dev/null; then
        afterburner_bar_dump > "$OUTPUT_DIR/bar_registers.txt" 2>&1
    elif command -v pcireg &> /dev/null; then
        # Use pcireg if available (from pciutils or similar)
        pcireg -d 106b:0205 > "$OUTPUT_DIR/bar_registers.txt" 2>&1 || \
            echo "(pcireg failed)"
    else
        cat << 'EOF'
(No BAR dump tool available. To capture BAR registers, you need to either:
 1. Build a small macOS kext or userspace tool that maps the PCI BARs and reads them
 2. Use `ioreg` to extract DMA command object properties (see ioreg_dma_commands.txt)
 3. Run this script on a Mac where you've already built a BAR dump helper

For now, the IOKit registry dump (step 2) should contain enough information
to understand the descriptor ring layout and doorbell mechanism.)
EOF
    fi
}

# ---------------------------------------------------------------------------
# 5. ProRes decode activity (if available)
# ---------------------------------------------------------------------------
echo "[5/8] Checking for ProRes decode activity..."
{
    echo "=== Active ProRes processes ==="
    ps aux | grep -i "prores\|afterburner" | grep -v grep > "$OUTPUT_DIR/prores_processes.txt" 2>&1 || \
        echo "(No active ProRes/Afterburner processes found)"
    
    echo ""
    echo "=== Video decode statistics (if available) ==="
    # Some macOS versions expose video decode stats via `system_profiler` or `powermetrics`
    if command -v powermetrics &> /dev/null; then
        powermetrics --samplers gpu_power -n 1 2>/dev/null | \
            grep -i "video\|decode\|prores" > "$OUTPUT_DIR/video_stats.txt" || \
            echo "(No video decode stats available)"
    else
        echo "(powermetrics not available or no video decode stats exposed)"
    fi
}

# ---------------------------------------------------------------------------
# 6. Kext load status and version
# ---------------------------------------------------------------------------
echo "[6/8] Checking kext load status..."
{
    echo "=== Loaded kexts (kextstat) ==="
    kextstat 2>/dev/null | grep -i "afterburner\|106b" > "$OUTPUT_DIR/kext_status.txt" || \
        echo "(No Afterburner kext found in kextstat)"
    
    echo ""
    echo "=== Kext info (if loaded) ==="
    if command -v kextdump &> /dev/null; then
        # Find the kext path and dump its info
        KEXT_PATH=$(kextstat 2>/dev/null | grep -i afterburner | awk '{print $NF}' | head -1)
        if [ -n "$KEXT_PATH" ] && [ -f "$KEXT_PATH" ]; then
            kextdump "$KEXT_PATH" > "$OUTPUT_DIR/kext_dump.txt" 2>&1 || \
                echo "(kextdump failed)"
        else
            echo "(Could not locate Afterburner kext file)"
        fi
    else
        echo "(kextdump not available)"
    fi
}

# ---------------------------------------------------------------------------
# 7. System configuration (PCIe topology, etc.)
# ---------------------------------------------------------------------------
echo "[7/8] Capturing system configuration..."
{
    echo "=== PCIe Topology ==="
    # macOS doesn't have lspci, but we can get some info from ioreg
    ioreg -l | grep -B 5 -A 20 "PCIe\|pci-bridge" > "$OUTPUT_DIR/pcie_topology.txt" 2>&1 || \
        echo "(Could not extract PCIe topology)"
    
    echo ""
    echo "=== Memory Info ==="
    sysctl hw.memsize 2>/dev/null | awk '{print $2 / 1024 / 1024 / 1024 " GB"}' > "$OUTPUT_DIR/memory_info.txt" || \
        echo "(Could not get memory info)"
    
    echo ""
    echo "=== CPU Info ==="
    sysctl machdep.cpu.brand_string 2>/dev/null >> "$OUTPUT_DIR/cpu_info.txt" || \
        echo "(Could not get CPU info)"
}

# ---------------------------------------------------------------------------
# 8. Package everything into a tarball
# ---------------------------------------------------------------------------
echo "[8/8] Packaging capture..."
cd "$(dirname "$OUTPUT_DIR")"
TARBALL_NAME="$(basename "$OUTPUT_DIR").tar.gz"
tar -czf "$TARBALL_NAME" "$OUTPUT_DIR"

echo ""
echo "=== Capture Complete ==="
echo "Output directory: $OUTPUT_DIR"
echo "Tarball: $(pwd)/$TARBALL_NAME"
echo ""
echo "Bring this tarball back to the Linux box for analysis."
echo "Key files to examine:"
echo "  - ioreg_afterburner.txt    (IOKit registry entries for the card)"
echo "  - ioreg_dma_commands.txt   (IODMACommand objects, if visible)"
echo "  - kernel_log.txt           (Afterburner-related kernel messages)"
echo "  - bar_registers.txt        (PCI BAR register dump, if tool available)"
