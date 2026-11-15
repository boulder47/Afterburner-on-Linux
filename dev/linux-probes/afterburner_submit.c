// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_submit.c — Phase 3b: FIRST REAL DMA SUBMIT (the critical moment).
 *
 * Per §7.4/§7.5 and the corrected geometry from §16, the submit path is:
 *   1. Allocate a coherent DMA buffer for the descriptor ring (system RAM, card-readable).
 *   2. Allocate a small test data buffer (the "frame" to decode — we use zeros for now;
 *      a real ProRes frame comes later once the pipeline is proven).
 *   3. Fill ONE 32-byte descriptor: next-ptr=0, data_pa = physical addr of test buffer,
 *      control = (len<<8) | 0xAD4B0000 | 0x13 (last-desc flags).
 *   4. Write the descriptor's physical address to the card's desc registers:
 *        bar0+0x5080 = pa_lo, bar0+0x5084 = pa_hi, bar0+0x5088 = mid (single-shot bank).
 *   5. Ring the single-shot doorbell: writel(0xFFE7F, bar0+0x1004).
 *   6. Poll for completion: read back the desc registers + status regs; also watch for an IRQ
 *      (MSI handler from Phase 2b is NOT registered here — we poll instead, per §7.7 fallback).
 *
 * SAFETY MODEL:
 *   - Tiny buffer (4 KB of zeros) — even if the card DMA-reads it, there's no meaningful data
 *     to corrupt and no write-back target (we don't set up an output buffer yet).
 *   - Single-shot doorbell (0xFFE7F), not batch — processes exactly one descriptor.
 *   - Bounded poll (max 2 s) with full register readback at each step, so if the card hangs or
 *     misbehaves we have complete state to diagnose from and can reset.
 *   - We do NOT set up an output buffer yet — this is a "does the pipeline accept and process a
 *     descriptor" test, not a full decode. If it works, Phase 3c adds the output path.
 *
 * This is the first time we write to the doorbell register. Per §14 Result 2, writes are safe
 * (the card doesn't hang on benign writes), and per §7.4 the single-shot GO is a well-defined op.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x205

/* Descriptor ring: 4 slots × 32 bytes = 128 bytes (we only use slot 0 for now). */
#define RING_SLOTS	4
#define DESC_SIZE	32
#define RING_SIZE	(RING_SLOTS * DESC_SIZE)

/* Test data buffer: 4 KB of zeros. Small enough to be harmless, large enough to be a valid DMA target. */
#define DATA_SIZE	(4 * 1024)

struct ab_submit {
	struct pci_dev *pdev;
	void __iomem *bar0;
	dma_addr_t ring_pa;      /* physical addr of descriptor ring */
	void *ring_va;           /* virtual addr of descriptor ring */
	dma_addr_t data_pa;      /* physical addr of test data buffer */
	void *data_va;           /* virtual addr of test data buffer */
};

static struct ab_submit ab;

/* Read back the key DMA registers after a submit, for diagnostics. */
static void dump_dma_regs(struct pci_dev *pdev)
{
	dev_info(&pdev->dev, "  [post-submit register readback]\n");
	dev_info(&pdev->dev, "    bar0+0x1004 (single doorbell) = 0x%08x\n", readl(ab.bar0 + 0x1004));
	dev_info(&pdev->dev, "    bar0+0x2004 (batch doorbell)  = 0x%08x\n", readl(ab.bar0 + 0x2004));
	dev_info(&pdev->dev, "    bar0+0x5080 (desc_lo)         = 0x%08x\n", readl(ab.bar0 + 0x5080));
	dev_info(&pdev->dev, "    bar0+0x5084 (desc_hi)         = 0x%08x\n", readl(ab.bar0 + 0x5084));
	dev_info(&pdev->dev, "    bar0+0x5088 (desc_mid)        = 0x%08x\n", readl(ab.bar0 + 0x5088));
	/* Status/completion registers: check the counter section and engine banks. */
	dev_info(&pdev->dev, "    bar0+0x2034 (counter slot 0 ctrl) = 0x%08x\n", readl(ab.bar0 + 0x2034));
	dev_info(&pdev->dev, "    bar0+0x8014 (engine bank 0 ctrl)  = 0x%08x\n", readl(ab.bar0 + 0x8014));
}

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int ret;

	pci_info(pdev, "Apple Afterburner [106b:0205] Phase 3b FIRST REAL DMA SUBMIT\n");

	ab.pdev = pdev;

	if (pci_enable_device(pdev)) return -ENODEV;
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_submit")) goto out_disable;

	ab.bar0 = pci_iomap(pdev, 0, 0);
	if (!ab.bar0) { dev_err(&pdev->dev, "iomap BAR0 failed\n"); goto out_release; }

	/* --- 1. Allocate coherent DMA buffer for the descriptor ring. --- */
	ab.ring_va = dma_alloc_coherent(&pdev->dev, RING_SIZE, &ab.ring_pa, GFP_KERNEL);
	if (!ab.ring_va) { dev_err(&pdev->dev, "dma_alloc_coherent (ring) failed\n"); goto out_unmap; }
	memset(ab.ring_va, 0, RING_SIZE);
	pci_info(pdev, "Ring allocated: va=%px pa=0x%llx size=%d bytes (%d slots)\n",
		 ab.ring_va, (unsigned long long)ab.ring_pa, RING_SIZE, RING_SLOTS);

	/* --- 2. Allocate the test data buffer (4 KB of zeros). --- */
	ab.data_va = dma_alloc_coherent(&pdev->dev, DATA_SIZE, &ab.data_pa, GFP_KERNEL);
	if (!ab.data_va) { dev_err(&pdev->dev, "dma_alloc_coherent (data) failed\n"); goto out_free_ring; }
	memset(ab.data_va, 0, DATA_SIZE);
	pci_info(pdev, "Data buffer allocated: va=%px pa=0x%llx size=%d bytes\n",
		 ab.data_va, (unsigned long long)ab.data_pa, DATA_SIZE);

	/* --- 3. Fill ONE descriptor in slot 0 of the ring. --- */
	{
		u8 *desc = ab.ring_va;  /* slot 0 is at offset 0 */
		/* +0x0: next-ptr (0 = end of chain) */
		writel(0, desc + 0x0);
		/* +0x8: physical address of the data buffer (64-bit) */
		writel((u32)(ab.data_pa & 0xffffffff), desc + 0x8);
		writel((u32)((ab.data_pa >> 32) & 0xffff), desc + 0xc);
		/* +0x10: control word = (len<<8) | 0xAD4B0000 | 0x13 (last-desc flags) */
		writel((DATA_SIZE << 8) | 0xAD4B0000 | 0x13, desc + 0x10);
		pci_info(pdev, "Descriptor filled: data_pa=0x%llx len=%d ctrl=0x%08x\n",
			 (unsigned long long)ab.data_pa, DATA_SIZE,
			 readl(desc + 0x10));
	}

	/* --- 4. Write the descriptor's physical address to the card. --- */
	pci_info(pdev, "Writing descriptor PA to card desc registers...\n");
	writel((u32)(ab.ring_pa & 0xffffffff), ab.bar0 + 0x5080);   /* desc_lo */
	writel((u32)((ab.ring_pa >> 32) & 0xffff), ab.bar0 + 0x5084); /* desc_hi */
	writel(0, ab.bar0 + 0x5088);                                 /* desc_mid (unused for <64-bit PA) */

	/* Verify the write took. */
	dev_info(&pdev->dev, "  desc_lo readback = 0x%08x (expected 0x%08x)\n",
		 readl(ab.bar0 + 0x5080), (u32)(ab.ring_pa & 0xffffffff));

	/* --- 5. Ring the single-shot doorbell. --- */
	pci_info(pdev, "Ringing single-shot doorbell: writel(0xFFE7F, bar0+0x1004)\n");
	writel(0xFFE7F, ab.bar0 + 0x1004);

	/* --- 6. Poll for completion (bounded, with readback). --- */
	pci_info(pdev, "Polling for completion (max 2s)...\n");
	{
		int i;
		u32 before = readl(ab.bar0 + 0x5080);
		for (i = 0; i < 100; i++) {
			msleep(20);
			u32 after = readl(ab.bar0 + 0x5080);
			if (after != before) {
				dev_info(&pdev->dev, "    ^ desc_lo changed at %d ms: 0x%08x -> 0x%08x\n",
					 i * 20, before, after);
				dump_dma_regs(pdev);
				break;
			}
			if (i == 49) { /* halfway mark */
				dev_info(&pdev->dev, "    ... still polling at 1s ...\n");
			}
		}
		if (i >= 100) {
			dev_info(&pdev->dev, "    (no desc_lo change within 2s — may need different completion signal)\n");
			dump_dma_regs(pdev);
		}
	}

	pci_info(pdev, "--- Phase 3b submit complete ---\n");

out_free_ring:
	dma_free_coherent(&pdev->dev, RING_SIZE, ab.ring_va, ab.ring_pa);
out_unmap:
	pci_iounmap(pdev, ab.bar0);
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
	.name	= "afterburner_submit",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 3b first real DMA submit");
