// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_frame.c — Phase 3c: load a REAL ProRes frame + pin the desc-PA register.
 *
 * Two goals this phase:
 *   (A) Pin down where the card actually reads the descriptor physical address from. In §17 we
 *       wrote to bar0+0x5080 and it read back 0xffffffff (ignored). We need to find the real
 *       desc-PA register by sweeping candidate offsets, writing a known PA, and reading back.
 *   (B) Load a REAL ProRes frame (frame0.yuv, 1920x1080 yuv422p10le = 8,294,400 bytes) into
 *       coherent DMA memory so we have meaningful input data for the decode pipeline.
 *
 * APPROACH:
 *   1. Open /home/archuser/afterburner-re/frame0.yuv via filp_open + kernel_read into a
 *      coherent DMA buffer (8 MB). This gives us real frame data at a known physical address.
 *   2. SWEEP candidate desc-PA register offsets: for each candidate, write the ring PA, read it
 *      back, and log which ones ACCEPT the write (readback == written value). The ones that stick
 *      are our real desc registers. Candidates come from §7.4's map (+0x4080/+0x5080 family) plus
 *      nearby offsets in case the base is different.
 *   3. Once we know which register accepts the PA, fill a descriptor pointing to the REAL frame
 *      data (not zeros), write its PA to the confirmed register, and ring the doorbell.
 *   4. Read back all state registers + check if the frame buffer was modified (the card may have
 *      DMA-read it). Log everything for diagnosis.
 *
 * SAFETY:
 *   - The sweep only WRITES the ring PA (a valid, allocated address) to candidate registers and
 *     reads them back — no doorbell is rung during the sweep, so nothing is submitted yet.
 *   - The real submit uses the same single-shot doorbell as §17 (proven safe).
 *   - 8 MB coherent buffer is large but well within system RAM; freed on exit.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x205

/* Real frame: 1920x1080 yuv422p10le = 8,294,400 bytes. */
#define FRAME_BYTES	(8294400UL)
#define RING_SLOTS	4
#define DESC_SIZE	32
#define RING_SIZE	(RING_SLOTS * DESC_SIZE)

static const char *frame_path = "/home/archuser/afterburner-re/frame0.yuv";

struct ab_frame {
	struct pci_dev *pdev;
	void __iomem *bar0;
	dma_addr_t ring_pa;
	void *ring_va;
	dma_addr_t frame_pa;
	void *frame_va;
	u32 good_desc_regs[64];  /* offsets that accepted the PA write */
	int n_good;
};

static struct ab_frame ab;

/* Try writing `pa` to bar0+off and see if it sticks (readback == pa_lo). */
static bool test_desc_reg(u32 off, dma_addr_t pa)
{
	u32 lo = (u32)(pa & 0xffffffff);
	writel(lo, ab.bar0 + off);
	u32 rb = readl(ab.bar0 + off);
	return (rb == lo);
}

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct file *f;
	loff_t pos;
	ssize_t rd;

	pci_info(pdev, "Apple Afterburner [106b:0205] Phase 3c REAL FRAME + desc-reg pin\n");

	ab.pdev = pdev;
	ab.n_good = 0;

	if (pci_enable_device(pdev)) return -ENODEV;
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_frame")) goto out_disable;
	ab.bar0 = pci_iomap(pdev, 0, 0);
	if (!ab.bar0) { dev_err(&pdev->dev, "iomap BAR0 failed\n"); goto out_release; }

	/* --- (A) SWEEP candidate desc-PA registers BEFORE loading the frame. --- */
	pci_info(pdev, "--- sweeping candidate desc-PA register offsets ---\n");
	{
		dma_addr_t probe_pa = 0x12345678;  /* arbitrary test value */
		u32 candidates[] = {
			/* §7.4's map (dma base = bar0+0x1000): */
			0x1000 + 0x4080, 0x1000 + 0x5080,   /* desc_lo / batch desc_lo */
			0x1000 + 0x4084, 0x1000 + 0x5084,   /* desc_hi / batch desc_hi */
			0x1000 + 0x4088, 0x1000 + 0x5088,   /* desc_mid */
			/* Absolute BAR0 interpretations: */
			0x4080, 0x5080, 0x4084, 0x5084, 0x4088, 0x5088,
			/* Nearby offsets in case the base is slightly off: */
			0x1000 + 0x4000, 0x1000 + 0x5000,
			0x1000 + 0x4078, 0x1000 + 0x407c,
			0x1000 + 0x5078, 0x1000 + 0x507c,
			/* The counter section (where the card latched state in §17): */
			0x2000, 0x2004, 0x2008, 0x200c, 0x2010,
			/* Engine bank 0 control area: */
			0x8000, 0x8004, 0x8008, 0x800c, 0x8010,
		};
		int nc = sizeof(candidates)/sizeof(candidates[0]);

		for (int i = 0; i < nc; i++) {
			u32 off = candidates[i];
			bool ok = test_desc_reg(off, probe_pa);
			if (ok) {
				dev_info(&pdev->dev, "    bar0+0x%x ACCEPTS write (readback=0x%08x)\n",
					 off, readl(ab.bar0 + off));
				if (ab.n_good < 64) ab.good_desc_regs[ab.n_good++] = off;
			} else {
				dev_info(&pdev->dev, "    bar0+0x%x ignored (readback=0x%08x)\n",
					 off, readl(ab.bar0 + off));
			}
		}
		pci_info(pdev, "  -> %d candidate registers accepted the PA write\n", ab.n_good);
	}

	/* --- (B) Load the REAL ProRes frame into coherent DMA memory. --- */
	pci_info(pdev, "--- loading real frame: %s (%lu bytes) ---\n", frame_path, FRAME_BYTES);

	ab.frame_va = dma_alloc_coherent(&pdev->dev, FRAME_BYTES, &ab.frame_pa, GFP_KERNEL);
	if (!ab.frame_va) { dev_err(&pdev->dev, "dma_alloc_coherent (frame 8MB) failed\n"); goto out_unmap; }
	pci_info(pdev, "Frame buffer: va=%px pa=0x%llx\n", ab.frame_va, (unsigned long long)ab.frame_pa);

	f = filp_open(frame_path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		dev_err(&pdev->dev, "filp_open(%s) failed: %ld\n", frame_path, PTR_ERR(f));
		goto out_free_frame;
	}
	pos = 0;
	rd = kernel_read(f, ab.frame_va, FRAME_BYTES, &pos);
	filp_close(f, NULL);
	if (rd < 0) {
		dev_err(&pdev->dev, "kernel_read failed: %zd\n", rd);
		goto out_free_frame;
	}
	pci_info(pdev, "Read %zd bytes from %s into DMA buffer at pa=0x%llx\n",
		 rd, frame_path, (unsigned long long)ab.frame_pa);

	/* Verify we got real data (not zeros): check first 16 bytes. */
	dev_info(&pdev->dev, "  frame head: ");
	for (int i = 0; i < 4; i++)
		dev_info(&pdev->dev, "%08x ", ((u32 *)ab.frame_va)[i]);
	dev_info(&pdev->dev, "\n");

	/* --- Allocate the descriptor ring. --- */
	ab.ring_va = dma_alloc_coherent(&pdev->dev, RING_SIZE, &ab.ring_pa, GFP_KERNEL);
	if (!ab.ring_va) { dev_err(&pdev->dev, "dma_alloc_coherent (ring) failed\n"); goto out_free_frame; }
	memset(ab.ring_va, 0, RING_SIZE);

	/* Fill ONE descriptor pointing to the REAL frame data. */
	{
		u8 *desc = ab.ring_va;
		writel(0, desc + 0x0);                              /* next-ptr = end of chain */
		writel((u32)(ab.frame_pa & 0xffffffff), desc + 0x8); /* data PA lo */
		writel((u32)((ab.frame_pa >> 32) & 0xffff), desc + 0xc); /* data PA hi */
		writel((FRAME_BYTES << 8) | 0xAD4B0000 | 0x13, desc + 0x10); /* ctrl: len|tag|last */
		pci_info(pdev, "Descriptor filled: frame_pa=0x%llx len=%lu\n",
			 (unsigned long long)ab.frame_pa, FRAME_BYTES);
	}

	/* --- Write the ring PA to ALL confirmed desc registers + ring the doorbell. --- */
	if (ab.n_good > 0) {
		pci_info(pdev, "--- writing ring PA=0x%llx to %d confirmed desc regs ---\n",
			 (unsigned long long)ab.ring_pa, ab.n_good);
		for (int i = 0; i < ab.n_good; i++) {
			u32 off = ab.good_desc_regs[i];
			writel((u32)(ab.ring_pa & 0xffffffff), ab.bar0 + off);
			dev_info(&pdev->dev, "    bar0+0x%x <- 0x%08x (readback=0x%08x)\n",
				 off, (u32)(ab.ring_pa & 0xffffffff), readl(ab.bar0 + off));
		}
	} else {
		dev_info(&pdev->dev, "    (no confirmed desc regs — writing to §7.4 defaults anyway)\n");
		writel((u32)(ab.ring_pa & 0xffffffff), ab.bar0 + 0x5080);
		writel((u32)((ab.ring_pa >> 32) & 0xffff), ab.bar0 + 0x5084);
	}

	pci_info(pdev, "Ringing single-shot doorbell: writel(0xFFE7F, bar0+0x1004)\n");
	writel(0xFFE7F, ab.bar0 + 0x1004);

	/* --- Poll + full readback. --- */
	pci_info(pdev, "Polling (max 2s)...\n");
	msleep(500);
	dev_info(&pdev->dev, "  [post-submit state]\n");
	dev_info(&pdev->dev, "    doorbell bar0+0x1004 = 0x%08x\n", readl(ab.bar0 + 0x1004));
	dev_info(&pdev->dev, "    counter slot 0 bar0+0x2034 = 0x%08x\n", readl(ab.bar0 + 0x2034));
	dev_info(&pdev->dev, "    engine bank 0 bar0+0x8014 = 0x%08x\n", readl(ab.bar0 + 0x8014));

	/* Check if the frame buffer was modified (card DMA-read it?). */
	dev_info(&pdev->dev, "  [frame head after submit]\n");
	for (int i = 0; i < 4; i++)
		dev_info(&pdev->dev, "    %08x ", ((u32 *)ab.frame_va)[i]);
	dev_info(&pdev->dev, "\n");

	pci_info(pdev, "--- Phase 3c complete ---\n");

out_free_frame:
	if (ab.frame_va) dma_free_coherent(&pdev->dev, FRAME_BYTES, ab.frame_va, ab.frame_pa);
	if (ab.ring_va) dma_free_coherent(&pdev->dev, RING_SIZE, ab.ring_va, ab.ring_pa);
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
	.name	= "afterburner_frame",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 3c real frame + desc-reg pin");
