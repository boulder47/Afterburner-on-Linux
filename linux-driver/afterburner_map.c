// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_map.c — Phase 0b: FULL BAR0 map for Apple Afterburner [106b:0205].
 *
 * Why: the first probe (afterburner_probe.c) read candidate bases at 0x130-0x190 and got
 * only uniform 0/1 noise — those offsets were the kext driver-object pointer slots, NOT
 * raw BAR offsets. The real register banks live elsewhere in BAR0. This module sweeps the
 * entire 128 KiB BAR0 (and a slice of BAR1) to locate structured register space.
 *
 * Method: read every 4-byte word across the BAR; log runs that are NOT pure 0x0/0xFFFFFFFF
 * and have some structure (mixed values). Also emits a compact "nonzero map" so we can see
 * at a glance which offset ranges contain live logic.
 *
 * SAFETY: read-only. No writes, no doorbells, no flash. Only enables device + ioremaps.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x0205

/* Log a word only if it's "interesting": not 0, not all-ones, and not the uniform 0/1 noise. */
static inline int interesting(u32 v)
{
	if (v == 0 || v == 0xFFFFFFFF) return 0;
	/* pure 0/1 bit pattern (the noise we saw) — skip unless it has higher bits set */
	if ((v & ~0x1u) == 0) return 0;   /* only bit0 may be set -> noise */
	return 1;
}

static void dump_bar(struct pci_dev *pdev, int bar, size_t len, const char *tag)
{
	void __iomem *base = pci_iomap(pdev, bar, 0);
	size_t i, n = len / 4;
	u32 v;
	int run_start = -1, run_len = 0;

	if (!base) { dev_err(&pdev->dev, "%s: iomap failed\n", tag); return; }

	dev_info(&pdev->dev, "=== %s BAR%d full map (%zu bytes / %zu words) ===\n",
		 tag, bar, len, n);

	for (i = 0; i < n; i++) {
		v = readl(base + i * 4);
		if (interesting(v)) {
			dev_info(&pdev->dev, "  BAR%d+0x%05zx: 0x%08x\n", bar, i*4, v);
			run_start = i; run_len = 1;
		} else if (run_start >= 0) {
			/* end of a run */
			if (run_len >= 2)
				dev_info(&pdev->dev, "    ^ run of %d interesting words ending here\n", run_len);
			run_start = -1;
		}
	}
	pci_iounmap(pdev, base);
}

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	u64 size0, size1;

	pci_info(pdev, "Apple Afterburner [106b:0205] Phase 0b full BAR map (read-only)\n");

	if (pci_enable_device(pdev)) return -ENODEV;
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_map")) goto out_disable;

	size0 = pci_resource_len(pdev, 0);
	size1 = pci_resource_len(pdev, 1);
	pci_info(pdev, "BAR0 size=%llu BAR1 size=%llu\n",
		 (unsigned long long)size0, (unsigned long long)size1);

	/* Full sweep of BAR0 (128K). */
	dump_bar(pdev, 0, size0, "FULL");

	/* First 4K of BAR1 to see if it has any structure at all. */
	if (size1)
		dump_bar(pdev, 1, min_t(u64, size1, 4096), "SLICE");

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
	.name	= "afterburner_map",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 0b full BAR map (read-only)");
