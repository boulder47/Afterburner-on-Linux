// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_flash2.c — Phase 2b: fine-grained Flash command-block locator + READ.
 *
 * Phase 2a (afterburner_flash.ko) established that the live BAR0 register space is
 * +0x8000..+0xf000 (reads clean zeros at rest; everything else reads 0xffffffff = unmapped).
 * The Flash cmd block (§9.1: busy@+0x10, addr@+0x40, cmd@+0x48, rdback@+0x50) lives inside
 * that window but my page-granularity scan was too coarse to pin it. This module does a
 * fine-grained sweep of +0x8000..+0xf000:
 *
 *   1. Read every word in the window (read-only) and log any non-zero — that reveals the
 *      exact sub-structure already present at rest.
 *   2. For each candidate base B in the window, issue the documented READ sequence
 *      (addr=0x13FE000 to B+0x40, cmd=0xBBBB to B+0x48, poll B+0x10&0x10 bounded, read
 *      bswap(B+0x50), deassert). If a non-trivial value comes back on the readback word,
 *      we've found the Flash base.
 *
 * SAFETY: identical to Phase 2a — only the documented non-destructive READ + cmd deassert.
 * Hard-bounded poll (<=16 iters x 1ms). No erase/program/doorbell.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/delay.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x205

#define F_BUSY		0x10
#define F_ADDR		0x40
#define F_CMD		0x48
#define F_READBACK	0x50

#define FLASH_ID_ADDR	0x13FE000u
#define CMD_READ	0xBBBBu

static u32 bswap32_k(u32 v) { return __swab32(v); }

/* Issue the READ at base B; returns readback (bswap). */
static u32 do_read(struct pci_dev *pdev, void __iomem *bar0, u32 B)
{
	u32 status, rb;
	int i;
	writel(FLASH_ID_ADDR, bar0 + B + F_ADDR);
	writel(CMD_READ,    bar0 + B + F_CMD);
	for (i = 0; i < 16; i++) {
		status = readl(bar0 + B + F_BUSY);
		if (!(status & 0x10)) break;
		msleep(1);
	}
	rb = bswap32_k(readl(bar0 + B + F_READBACK));
	writel(0, bar0 + B + F_CMD);   /* deassert */
	return rb;
}

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	void __iomem *bar0;
	u32 id_word;

	pci_info(pdev, "Apple Afterburner [106b:0205] Phase 2b fine-grained Flash locator + READ\n");

	if (pci_enable_device(pdev)) return -ENODEV;
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_flash2")) goto out_disable;
	bar0 = pci_iomap(pdev, 0, 0);
	if (!bar0) { dev_err(&pdev->dev, "iomap BAR0 failed\n"); goto out_release; }

	id_word = readl(bar0 + 0x4);
	pci_info(pdev, "ID block +0x4 = 0x%08x (reference)\n", id_word);

	/* Step 1: read-only sweep of the live window, log non-zero words. */
	pci_info(pdev, "--- step 1: read-only sweep +0x8000..+0xf000 (non-zero only) ---\n");
	for (u32 o = 0x8000; o < 0xf000; o += 4) {
		u32 v = readl(bar0 + o);
		if (v != 0 && v != 0xffffffff)
			dev_info(&pdev->dev, "  +0x%x: 0x%08x\n", o, v);
	}

	/* Step 2: try the READ at each candidate base in the window. */
	pci_info(pdev, "--- step 2: issue READ at each candidate base (non-destructive) ---\n");
	for (u32 B = 0x8000; B + 0x54 <= 0xf000; B += 4) {
		u32 rb = do_read(pdev, bar0, B);
		if (rb != 0 && rb != 0xffffffff) {
			dev_info(&pdev->dev, "    ^ READBACK at base +0x%x: 0x%08x\n", B, rb);
		}
	}
	pci_info(pdev, "--- flash2 scan done ---\n");

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
	.name	= "afterburner_flash2",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 2b fine-grained Flash locator + READ");
