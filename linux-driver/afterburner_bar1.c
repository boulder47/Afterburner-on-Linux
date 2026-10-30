// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_bar1.c — Phase 3d: map BAR1 (the REAL DMA register space) + full dump.
 *
 * BREAKTHROUGH from kext disassembly (§19): the kext maps TWO BARs in start():
 *   - BAR0 -> this+0x128  (writeBAR0Register / readBAR0Register)
 *   - BAR1 -> this+0x130  (writeDMARegister / readDMARegister)
 *
 * Our Linux driver only mapped BAR0. ALL the "DMA registers" from §7.4 (doorbells at +0x4/+0x1004,
 * desc_lo/desc_hi at +0x4080/+0x5080) live in BAR1, NOT BAR0. That's why Phase 3c's sweep of BAR0
 * offsets found nothing — we were writing to the wrong physical location entirely.
 *
 * This module maps BAR1 (64K) and does a full dump + identifies the DMA register layout by reading
 * the same §7.4 offsets against BAR1. READ-ONLY: no writes, just map + read + log.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x205

static const char *out_path = "/tmp/bar1_dump.txt";

/* Write a line to the output file (kernel-side). */
static void log_line(const char *fmt, ...)
{
	static struct file *f;
	static loff_t pos;
	char buf[256];
	va_list ap;
	int len;

	if (!f) {
		f = filp_open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (IS_ERR(f)) return;
		pos = 0;
	}
	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	kernel_write(f, buf, len, &pos);
}

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	void __iomem *bar1;
	u32 size;

	log_line("Apple Afterburner [106b:0205] Phase 3d BAR1 (DMA register space) full dump\n");

	if (pci_enable_device(pdev)) { log_line("pci_enable_device FAILED\n"); return -ENODEV; }
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_bar1")) { log_line("request_regions FAILED\n"); goto out_disable; }

	/* Map BAR1 (index 1). */
	size = pci_resource_len(pdev, 1);
	log_line("BAR1 size = %u bytes\n", size);
	bar1 = pci_iomap(pdev, 1, size);
	if (!bar1) { log_line("iomap BAR1 FAILED\n"); goto out_release; }

	/* --- Read the §7.4 DMA register offsets against BAR1. --- */
	log_line("--- §7.4 DMA registers (now against BAR1) ---\n");
	log_line("  bar1+0x4     (single doorbell dma[+0x4])   = 0x%08x\n", readl(bar1 + 0x4));
	log_line("  bar1+0x1004  (batch doorbell dma[+0x1004]) = 0x%08x\n", readl(bar1 + 0x1004));
	log_line("  bar1+0x4080  (desc_lo)                     = 0x%08x\n", readl(bar1 + 0x4080));
	log_line("  bar1+0x4084  (desc_hi)                     = 0x%08x\n", readl(bar1 + 0x4084));
	log_line("  bar1+0x5080  (batch desc_lo)               = 0x%08x\n", readl(bar1 + 0x5080));
	log_line("  bar1+0x5084  (batch desc_hi)               = 0x%08x\n", readl(bar1 + 0x5084));

	/* --- Full BAR1 dump: every non-trivial word. --- */
	log_line("--- full BAR1 dump (non-trivial words only) ---\n");
	{
		int shown = 0;
		for (u32 o = 0; o < size && shown < 200; o += 4) {
			u32 v = readl(bar1 + o);
			if (v != 0 && v != 0xffffffff) {
				log_line("    bar1+0x%04x = 0x%08x\n", o, v);
				shown++;
			}
		}
		log_line("  (%d non-trivial words shown)\n", shown);
	}

	/* --- Also dump the first 64 words of BAR1 unconditionally (header region). --- */
	log_line("--- BAR1 header (first 64 words) ---\n");
	for (u32 o = 0; o < 0x100; o += 4) {
		log_line("    bar1+0x%04x = 0x%08x\n", o, readl(bar1 + o));
	}

	log_line("--- Phase 3d BAR1 dump complete ---\n");
	pci_iounmap(pdev, bar1);
out_release:
	pci_release_regions(pdev);
out_disable:
	pci_disable_device(pdev);
	return 0;
}

static void ab_remove(struct pci_dev *pdev) { }

static const struct pci_device_id ab_pci_ids[] = {
	{ PCI_DEVICE(AB_VENDOR, AB_DEVICE) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, ab_pci_ids);

static struct pci_driver ab_driver = {
	.name	= "afterburner_bar1",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 3d BAR1 DMA register space dump");
