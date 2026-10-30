// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_probe.c — Phase 0 READ-ONLY bring-up for the Apple Afterburner card
 * [106b:0205] @ 0f:00.0.
 *
 * Purpose (per plan.md §4 Phase 0):
 *   - enable the device, request regions, ioremap BAR0 + BAR1
 *   - read the Version region (board rev / S/N / image revision+type) — safe liveness probe
 *   - read Thermal ADCs — confirms MMIO works
 *   - bounded scan of both BARs to locate which base holds our RE'd register regions
 *     (resolves the BAR->region map that FINDINGS.md left open)
 *
 * SAFETY: this module performs NO writes to the card. No doorbells, no flash, no DMA.
 * It only enables the device + maps its BARs (which any driver must do anyway) and reads.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/delay.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x0205

/*
 * Candidate region base offsets (from FINDINGS.md §5-9). These were relative to the
 * kext driver object's region pointers, NOT raw BAR offsets — so we scan for them.
 * Each is a 32-bit MMIO register bank; reading +0x0 of each should return something
 * plausible (not all-0s / not all-Fs) if that base really lives in this BAR.
 */
struct ab_region {
	const char *name;
	u32 off;
};

static const struct ab_region ab_regions[] = {
	{ "BAR0",     0x140 },
	{ "DMA",      0x130 },
	{ "Decode",   0x160 },
	{ "Flash",    0x150 },
	{ "Bitfile",  0x170 },
	{ "Thermal",  0x180 },
	{ "Version",  0x190 },
};

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	void __iomem *bar[2];
	int b;
	u32 i;
	u64 size;

	pci_info(pdev, "Apple Afterburner [106b:0205] probe (Phase 0 read-only)\n");

	if (pci_enable_device(pdev)) {
		pci_err(pdev, "pci_enable_device failed\n");
		return -ENODEV;
	}
	pci_set_master(pdev); /* enable BusMaster — needed for DMA later; harmless now */

	if (pci_request_regions(pdev, "afterburner")) {
		pci_err(pdev, "pci_request_regions failed\n");
		goto out_disable;
	}

	/* Map BAR0 and BAR1 */
	for (b = 0; b < 2; b++) {
		size = pci_resource_len(pdev, b);
		if (!size) { bar[b] = NULL; continue; }
		bar[b] = pci_iomap(pdev, b, 0);
		if (!bar[b]) {
			pci_err(pdev, "pci_iomap BAR%d failed\n", b);
			goto out_release;
		}
		pci_info(pdev, "BAR%d: phys=0x%llx size=%llu bytes (ioremap ok)\n",
			 b, (unsigned long long)pci_resource_start(pdev, b),
			 (unsigned long long)size);
	}

	/*
	 * Bounded scan: for each BAR, read +0x0 of every candidate region base and log it.
	 * A real register bank returns a stable, non-trivial value; unmapped/absent space
	 * typically reads 0x0 or 0xFFFFFFFF (bus error). This tells us which BAR holds the
	 * regions AND whether our offsets are raw-BAR-relative or need an extra base.
	 */
	for (b = 0; b < 2; b++) {
		if (!bar[b]) continue;
		pci_info(pdev, "--- scan BAR%d ---\n", b);
		for (i = 0; i < ARRAY_SIZE(ab_regions); i++) {
			u32 v = readl(bar[b] + ab_regions[i].off);
			pci_info(pdev, "  BAR%d+%04x %-8s -> 0x%08x\n",
				 b, ab_regions[i].off, ab_regions[i].name, v);
		}
	}

	/*
	 * Now try the Version region specifically on whichever base looks alive.
	 * FINDINGS.md §5: version bank holds board revision word + S/N string + image rev/type.
	 * We don't yet know its exact internal layout, so dump a small window of candidate
	 * bases that read non-trivially. (Refined in Phase 1 once the map is known.)
	 */
	for (b = 0; b < 2; b++) {
		if (!bar[b]) continue;
		for (i = 0; i < ARRAY_SIZE(ab_regions); i++) {
			u32 base_v = readl(bar[b] + ab_regions[i].off);
			if (base_v == 0 || base_v == 0xFFFFFFFF)
				continue; /* absent */
			pci_info(pdev, "--- window dump BAR%d+%04x (%s), 16 words ---\n",
				 b, ab_regions[i].off, ab_regions[i].name);
			{
				u32 w;
				for (w = 0; w < 16; w++)
					pci_info(pdev, "    +0x%02x: 0x%08x\n",
						 w * 4, readl(bar[b] + ab_regions[i].off + w * 4));
			}
		}
	}

out_release:
	for (b = 0; b < 2; b++)
		if (bar[b]) pci_iounmap(pdev, bar[b]);
	pci_release_regions(pdev);
out_disable:
	pci_disable_device(pdev);
	return 0; /* read-only probe: always "succeed", we just want the log */
}

static void ab_remove(struct pci_dev *pdev)
{
	pci_info(pdev, "Apple Afterburner removed\n");
}

static const struct pci_device_id ab_pci_ids[] = {
	{ PCI_DEVICE(AB_VENDOR, AB_DEVICE) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, ab_pci_ids);

static struct pci_driver ab_driver = {
	.name	= "afterburner_probe",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("afterburner-re");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 0 read-only probe");
