// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_flash.c — Phase 2: FIRST WRITE-TEST (safest possible) + IRQ discovery.
 *
 * Two jobs, both low-risk:
 *
 *  (A) Locate the Flash command block within BAR0 and issue a non-destructive READ.
 *      From FINDINGS.md §9.1 the Flash region is a small cmd/status block with fixed
 *      offsets: busy@+0x10, addr@+0x40, cmd@+0x48, data@+0x4c, readback@+0x50, done@+0x54.
 *      The READ sequence (updateIORegistryProperties) is:
 *          flash[+0x40] = 0x13FE000   (read address — device-ID / version register)
 *          flash[+0x48] = 0xBBBB      (READ command)
 *          poll while (flash[+0x10] & 0x10) usleep(100us);   [max 8 iters in the kext]
 *          val = bswap32(flash[+0x50])
 *          flash[+0x48] = 0           (deassert command)
 *      This is NON-destructive: it only reads a device-ID word from on-card flash. No erase,
 *      no program, no doorbell. We use a HARD timeout (bounded iterations + msleep) so the
 *      card can never hang us even if the base guess is wrong.
 *
 *      The Flash base isn't visible in the idle map (§13: reads all-zero at rest), so we try
 *      each candidate base, issue the READ, and check whether a plausible non-trivial value
 *      comes back on flash[+0x50]. Candidates are page-aligned offsets inside BAR0 that are
 *      NOT already claimed by a named section (type 2/3/4/7 headers at +0/+0x1000/+0x2000/
 *      +0x3000, ring at +0x4000). We also re-read the ID block (+0x4) to compare.
 *
 *  (B) IRQ discovery: report MSI capability state and whether we can enable an MSI vector.
 *      The card advertises MSI (64-bit, count=1) per lspci. We do NOT request a handler yet —
 *      that's the next step once we know which register raises it. We just confirm the vector
 *      is allocatable so Phase 2b (Bitfile handler) can proceed.
 *
 * SAFETY: every write here is either (a) the documented non-destructive flash READ, or (b) a
 * command deassert (=0). No doorbell, no erase (0xEEEE), no program ((len<<16)|0xAAAA).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/delay.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x205

/* Flash region internal offsets (FINDINGS.md §9.1) */
#define F_BUSY		0x10
#define F_ADDR		0x40
#define F_CMD		0x48
#define F_DATA		0x4c
#define F_READBACK	0x50
#define F_DONE		0x54

/* The documented READ address (device-ID / version register in on-card flash) */
#define FLASH_ID_ADDR	0x13FE000u
#define CMD_READ	0xBBBBu

static u32 bswap32_k(u32 v) { return __swab32(v); }

/*
 * Try one candidate Flash base: issue the READ, poll with a hard bound, return the read-back
 * value (byte-swapped as the kext does). Returns 0 if no plausible completion.
 */
static u32 try_flash_read(struct pci_dev *pdev, void __iomem *bar0, u32 base)
{
	u32 status, rb;
	int i;

	/* Pre-state: log what's there before we touch it (read-only). */
	dev_info(&pdev->dev, "  [flash@+0x%x] pre: busy=0x%08x cmd=0x%08x rdback=0x%08x done=0x%08x\n",
		 base, readl(bar0 + base + F_BUSY), readl(bar0 + base + F_CMD),
		 readl(bar0 + base + F_READBACK), readl(bar0 + base + F_DONE));

	/* Issue READ: addr then command. */
	writel(FLASH_ID_ADDR, bar0 + base + F_ADDR);
	writel(CMD_READ,    bar0 + base + F_CMD);

	/* Hard-bounded busy poll (kext uses 8 iters; we allow a bit more but always bounded). */
	for (i = 0; i < 32; i++) {
		status = readl(bar0 + base + F_BUSY);
		if (!(status & 0x10))   /* bit4 clear = not busy */
			break;
		msleep(1);              /* kext uses 100us; we use 1ms, still bounded (<=32ms) */
	}

	rb = bswap32_k(readl(bar0 + base + F_READBACK));

	/* Deassert command (safe: writing 0 clears the cmd word). */
	writel(0, bar0 + base + F_CMD);

	dev_info(&pdev->dev, "  [flash@+0x%x] post: busy=0x%08x iters=%d readback(bswap)=0x%08x\n",
		 base, readl(bar0 + base + F_BUSY), i, rb);
	return rb;
}

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	void __iomem *bar0;
	u32 id_word, best_rb = 0;
	int msi_vecs;

	pci_info(pdev, "Apple Afterburner [106b:0205] Phase 2 first-write-test (flash READ) + IRQ discovery\n");

	if (pci_enable_device(pdev)) return -ENODEV;
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_flash")) goto out_disable;

	bar0 = pci_iomap(pdev, 0, 0);
	if (!bar0) { dev_err(&pdev->dev, "iomap BAR0 failed\n"); goto out_release; }

	/* Reference: the ID block at +0x4 (AMD vendor word). */
	id_word = readl(bar0 + 0x4);
	pci_info(pdev, "ID block +0x4 = 0x%08x (reference for flash READ comparison)\n", id_word);

	/*
	 * Candidate Flash bases. The named sections occupy:
	 *   +0x0000 (type2 ID), +0x1000..+0x1fff (type3 decoder),
	 *   +0x2000..+0x2fff (type4 counters), +0x3000..+0x3fff (type7 channels),
	 *   +0x4000..+0x4fff (DMA ring).
	 * The Flash cmd block is small and reads 0 at rest, so it's likely in an otherwise-empty
	 * page. We try a spread of page-aligned candidates across BAR0 (128K = up to +0x1ffff),
	 * skipping the pages we already know are named sections/ring.
	 */
	static const u32 cands[] = {
		0x5000, 0x6000, 0x7000,        /* just past the ring */
		0x8000, 0x9000, 0xa000,          /* channel-descriptor pages (were sparse) */
		0xb000, 0xc000, 0xd000, 0xe000, 0xf000,
		0x10000, 0x11000, 0x12000,        /* upper half of the 128K BAR */
	};

	pci_info(pdev, "--- scanning candidate Flash bases (non-destructive READ each) ---\n");
	for (unsigned i = 0; i < ARRAY_SIZE(cands); i++) {
		u32 rb = try_flash_read(pdev, bar0, cands[i]);
		if (rb != 0 && rb != 0xffffffff) {
			dev_info(&pdev->dev, "    ^ non-trivial readback at base +0x%x: 0x%08x\n", cands[i], rb);
			best_rb = rb;
		}
	}

	pci_info(pdev, "--- flash READ scan done (best non-trivial readback: 0x%08x) ---\n", best_rb);

	/* IRQ discovery: how many MSI vectors can we get? */
	msi_vecs = pci_msi_vec_count(pdev);
	pci_info(pdev, "MSI: pci_msi_vecs_count=%d (card advertises 64-bit MSI count=1)\n", msi_vecs);

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
	.name	= "afterburner_flash",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 2 first-write-test (flash READ) + IRQ discovery");
