// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_name.c — Phase 1: NAME each register family by matching its signature
 * registers in the live BAR0 map.
 *
 * From FINDINGS.md §7-9 we know each family's INTERNAL layout (offsets relative to that
 * family's own base). The kext region bases are sub-windows into BAR0, not all at 0. So
 * for each family we dump its signature window and look for the tell-tale pattern:
 *
 *   DMA     : status/doorbell regs at +0x4 (single) / +0x1004 (batch); desc banks +0x4080/+0x5080
 *   Decode  : status bit0-ready at +0x8, doorbell at +0xc, "submitting" marker at +0x18
 *   Bitfile : int status bit0-pending at +0x14, secondary +0x10, ack reg +0x20, clear ctrl +0x4
 *   Flash   : busy flags +0x10, cmd addr +0x40, cmd word +0x48, data +0x4c, readback +0x50, done +0x54
 *   Thermal : six ADC words at +0x10,+0x80..+0x90 (last five >>7 fixed-point)
 *   Version : ID word at +0x4 (low nibble -> variant), bits16-31 logged separately
 *
 * Strategy: we already know from Phase 0 that the main region is BAR0+0x1000 and there are
 * 8 channel blocks at 0x8000..0xf000. We dump candidate base windows across BAR0 and let
 * the signature patterns reveal which offset is which family. Read-only, no writes.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x205

/* A signature window: (label, base_offset_in_BAR0, num_words) */
struct win { const char *label; u32 off; int n; };

static void dump_win(struct pci_dev *pdev, void __iomem *bar0, const struct win *w)
{
	int i;
	dev_info(&pdev->dev, "  [%s] BAR0+0x%04x (%d words):\n", w->label, w->off, w->n);
	for (i = 0; i < w->n; i++)
		dev_info(&pdev->dev, "    +0x%03x: 0x%08x\n",
			 w->off + i*4, readl(bar0 + w->off + i*4));
}

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	void __iomem *bar0;
	u64 size = pci_resource_len(pdev, 0);

	pci_info(pdev, "Apple Afterburner [106b:0205] Phase 1 family-naming probe (read-only)\n");

	if (pci_enable_device(pdev)) return -ENODEV;
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_name")) goto out_disable;

	bar0 = pci_iomap(pdev, 0, 0);
	if (!bar0) { dev_err(&pdev->dev, "iomap BAR0 failed\n"); goto out_release; }
	pci_info(pdev, "BAR0 size=%llu\n", (unsigned long long)size);

	/*
	 * Candidate family bases. From Phase 0 the live region is 0x1000-0x4fff plus the
	 * channel array at 0x8000+. We probe a spread of candidate bases; the signature
	 * patterns (doorbell slots, busy flags, ADC fixed-point values) will identify each.
	 */
	static const struct win wins[] = {
		/* Header / ID block */
		{ "HDR",     0x0000, 4 },

		/* Main region start — likely Version + control */
		{ "MAIN+0",  0x1000, 32 },

		/* Decode candidate: status@+8 (bit0 ready), doorbell@+c, marker@+18 */
		{ "DEC?",    0x1040, 16 },

		/* Bitfile candidate: int-status@+14, sec@+10, ack@+20, clear@+4 */
		{ "BIT?",    0x1080, 32 },

		/* Flash candidate: busy@+10, addr@+40, cmd@+48, data@+4c, rd@+50, done@+54 */
		{ "FLASH?",  0x1100, 64 },

		/* Thermal candidate: ADCs at +0x10,+0x80..+0x90 (dump a wide window) */
		{ "THM?",    0x1200, 64 },

		/* DMA candidate: doorbell@+4/+0x1004, desc banks +0x4080/+0x5080 — dump near-bank */
		{ "DMA-lo",   0x1300, 32 },
		{ "DMA-4k",   0x4000, 64 },   /* around +0x4080 desc bank if base~0x0 */
		{ "DMA-5k",   0x5000, 64 },   /* around +0x5080 batch bank */

		/* Channel array first block (the 8 identical descriptors) */
		{ "CHAN0",    0x8000, 16 },
	};

	for (unsigned i = 0; i < ARRAY_SIZE(wins); i++)
		dump_win(pdev, bar0, &wins[i]);

	pci_iounmap(pdev, bar0);
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
	.name	= "afterburner_name",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 1 family-naming probe (read-only)");
