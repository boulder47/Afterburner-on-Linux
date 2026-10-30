// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_dma.c — Phase 3a: DMA ring geometry probe (READ-ONLY, no doorbell).
 *
 * Before we write any descriptors or ring a doorbell, we must resolve the ring geometry:
 *   - §7.4/§7.5 say the descriptor ring is SYSTEM RAM coherent memory (kext allocates via
 *     IODMACommand), with 32-byte descriptors, tag 0xAD4B, head ptr at region +0xc8.
 *   - Phase 1 found a uniform 0x10000106 array at BAR0 +0x4000..+0x4fff (1024 words).
 *
 * This module reads (never writes) the candidate ring locations and the DMA doorbell/status
 * registers to determine:
 *   1. Is BAR0 +0x4000 a real descriptor ring, or just an idle tag array?
 *      - Read all 1024 words; if they're uniform 0x10000106 it's the "empty" state.
 *      - Look for a head/tail pointer register nearby (+0xc8 relative to some base).
 *   2. Where are the DMA doorbell/status registers? §7.4 says dma[+0x4] (single) and
 *      dma[+0x1004] (batch), with desc banks at +0x4080/+0x5080. Read those to see their
 *      idle values.
 *   3. Is there a ring-control block (head/tail/size) anywhere in the live region?
 *
 * SAFETY: pure reads. No doorbell, no descriptor write, no state change. This is the same
 * class of operation as Phase 0/1 probes — cannot alter card behavior.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x205

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	void __iomem *bar0;
	u32 id_word;

	pci_info(pdev, "Apple Afterburner [106b:0205] Phase 3a DMA ring geometry probe (READ-ONLY)\n");

	if (pci_enable_device(pdev)) return -ENODEV;
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_dma")) goto out_disable;
	bar0 = pci_iomap(pdev, 0, 0);
	if (!bar0) { dev_err(&pdev->dev, "iomap BAR0 failed\n"); goto out_release; }

	id_word = readl(bar0 + 0x4);
	pci_info(pdev, "ID block +0x4 = 0x%08x (sanity check)\n", id_word);

	/* --- 1. The candidate ring at BAR0 +0x4000..+0x4fff: read all 1024 words. --- */
	pci_info(pdev, "--- BAR0 +0x4000..+0x4fff (candidate descriptor ring) ---\n");
	{
		u32 first = readl(bar0 + 0x4000);
		u32 distinct = 1;
		u32 vals[8] = { first };
		int nv = 1;
		for (u32 o = 0x4004; o < 0x5000; o += 4) {
			u32 v = readl(bar0 + o);
			if (v != first) {
				distinct++;
				if (nv < 8) vals[nv++] = v;
			}
		}
		pci_info(pdev, "  first word @+0x4000 = 0x%08x\n", first);
		pci_info(pdev, "  distinct values across 1024 words: %u\n", distinct);
		for (int i = 0; i < nv; i++)
			dev_info(&pdev->dev, "    value[%d] = 0x%08x\n", i, vals[i]);

		/* Show the first and last few words to see if there's structure at the edges. */
		pci_info(pdev, "  head: ");
		for (u32 o = 0x4000; o < 0x4020; o += 4)
			dev_info(&pdev->dev, "%08x ", readl(bar0 + o));
		dev_info(&pdev->dev, "\n");
		pci_info(pdev, "  tail: ");
		for (u32 o = 0x4fe0; o < 0x5000; o += 4)
			dev_info(&pdev->dev, "%08x ", readl(bar0 + o));
		dev_info(&pdev->dev, "\n");
	}

	/* --- 2. DMA doorbell/status registers (§7.4): dma[+0x4], dma[+0x1004], desc banks. --- */
	pci_info(pdev, "--- DMA doorbell/status registers (idle values) ---\n");
	{
		/* The DMA region base is ambiguous: §7.4 uses self->+0x130 as the base with
		 * offsets +0x4/+0x1004/+0x4080/+0x5080. In our verified map the "main register
		 * region" starts at BAR0 +0x1000, so the DMA base is likely bar0+0x1000 (making
		 * dma[+0x4] = bar0+0x1004) OR bar0 itself. Read both interpretations. */

		/* Interpretation A: DMA base = bar0 + 0x1000 (main region). */
		dev_info(&pdev->dev, "  [A: dma_base=bar0+0x1000]\n");
		dev_info(&pdev->dev, "    dma[+0x4]   (single doorbell) = 0x%08x\n", readl(bar0 + 0x1000 + 0x4));
		dev_info(&pdev->dev, "    dma[+0x1004] (batch doorbell) = 0x%08x\n", readl(bar0 + 0x1000 + 0x1004));
		dev_info(&pdev->dev, "    dma[+0x4080] desc_lo          = 0x%08x\n", readl(bar0 + 0x1000 + 0x4080));
		dev_info(&pdev->dev, "    dma[+0x5080] batch desc_lo    = 0x%08x\n", readl(bar0 + 0x1000 + 0x5080));

		/* Interpretation B: DMA base = bar0 (offsets are absolute BAR0). */
		dev_info(&pdev->dev, "  [B: dma_base=bar0]\n");
		dev_info(&pdev->dev, "    bar0+0x4     = 0x%08x\n", readl(bar0 + 0x4));
		dev_info(&pdev->dev, "    bar0+0x1004  = 0x%08x\n", readl(bar0 + 0x1004));
		dev_info(&pdev->dev, "    bar0+0x4080  = 0x%08x\n", readl(bar0 + 0x4080));
		dev_info(&pdev->dev, "    bar0+0x5080  = 0x%08x (likely unmapped)\n", readl(bar0 + 0x5080));

		/* Interpretation C: the ring at +0x4000 IS the DMA region; doorbell is near it. */
		dev_info(&pdev->dev, "  [C: ring@bar0+0x4000, look for ctrl regs around it]\n");
		for (u32 o = 0x3f80; o < 0x5000 && o >= 0x3f80; o += 4) {
			u32 v = readl(bar0 + o);
			if (v != 0 && v != 0xffffffff && v != 0x10000106)
				dev_info(&pdev->dev, "    bar0+0x%x = 0x%08x\n", o, v);
		}
	}

	/* --- 3. Look for a ring-control block (head/tail/size) in the live region. --- */
	pci_info(pdev, "--- scanning +0x1000..+0x4fff for non-trivial control words (ring ctrl?) ---\n");
	{
		int shown = 0;
		for (u32 o = 0x1000; o < 0x4000 && shown < 40; o += 4) {
			u32 v = readl(bar0 + o);
			if (v != 0 && v != 0xffffffff) {
				dev_info(&pdev->dev, "    bar0+0x%x = 0x%08x\n", o, v);
				shown++;
			}
		}
		pci_info(pdev, "  (showing up to 40 non-trivial words in +0x1000..+0x3fff)\n");
	}

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
	.name	= "afterburner_dma",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 3a DMA ring geometry probe (read-only)");
