// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_full.c — Phase 1b: dump the ENTIRE live region BAR0+0x1000..0x4fff
 * INCLUDING zeros, so we can see the full register structure (idle status regs read 0).
 * Also re-dump the header (+0x0) and one channel block for completeness.
 * Read-only. Output is compact: 8 words per line.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x205

static void dump_line(struct pci_dev *pdev, void __iomem *bar0, u32 off)
{
	u32 w[8];
	int i;
	for (i = 0; i < 8; i++)
		w[i] = readl(bar0 + off + i*4);
	dev_info(&pdev->dev, "  %05x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		 off, w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7]);
}

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	void __iomem *bar0;
	u32 off;

	pci_info(pdev, "Apple Afterburner [106b:0205] Phase 1b FULL region dump (read-only)\n");

	if (pci_enable_device(pdev)) return -ENODEV;
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_full")) goto out_disable;

	bar0 = pci_iomap(pdev, 0, 0);
	if (!bar0) { dev_err(&pdev->dev, "iomap BAR0 failed\n"); goto out_release; }

	/* Header block */
	pci_info(pdev, "--- header +0x0000 ---\n");
	dump_line(pdev, bar0, 0x0);

	/* Full live region 0x1000..0x4fff (16 KiB = 4096 words = 512 lines) */
	pci_info(pdev, "--- FULL BAR0+0x1000 .. +0x4fff ---\n");
	for (off = 0x1000; off < 0x5000; off += 32)
		dump_line(pdev, bar0, off);

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
	.name	= "afterburner_full",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 1b full region dump (read-only)");
