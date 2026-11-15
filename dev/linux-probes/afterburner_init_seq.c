// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_init_seq.c — Phase 3g: Replicate kext's exact init sequence.
 *
 * From disassembly at 0x213c-0x21ba, the kext does:
 *   1. Write interrupt masks to BAR1+0x90 and BAR1+0x1090 (value 0xfffffe)
 *   2. Write interrupt config to BAR1+0x2010 (value 0x3)
 *   3. Fill a descriptor ring buffer with a specific pattern:
 *        +0x40: 0x2d002a80
 *        +0x48: 0x2d002a80
 *        +0x4c: 0x2a802800
 *        +0x50: 0x2d002a80
 *        +0x54: 0xbcd3ba4d  (magic "MBAK")
 *        +0x58: 0x2f802d00
 *        +0x60: 0x2f802d00
 *        +0x64: 0x2d002a80
 *        +0x68: 0x2f802d00
 *        +0x6c: 0xbf58bcd3  (magic "DCKB")
 *   4. Fill a secondary config buffer:
 *        +0x2c: 0x0
 *        +0x30: 0x6
 *        +0x34: 0x0
 *   5. Write the desc ring's PA to BAR1+0x3080/0x3084 (inferred from Phase 3e)
 *   6. Ring doorbell at BAR1+0x2004 (value 0x16)
 *
 * This test replicates steps 1-6 exactly and checks if the card responds.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x205

static const char *out_path = "/tmp/init_seq.txt";

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
	dma_addr_t dma_handle_desc, dma_handle_cfg;
	u8 *desc_buf, *cfg_buf;
	const u32 DESC_SIZE = 4096; /* 4KB for desc ring */
	const u32 CFG_SIZE = 4096;   /* 4KB for config buffer */

	log_line("=== Phase 3g: Replicate kext init sequence ===\n\n");

	if (pci_enable_device(pdev)) { log_line("pci_enable_device FAILED\n"); return -ENODEV; }
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_init_seq")) { log_line("request_regions FAILED\n"); goto out_disable; }

	size = pci_resource_len(pdev, 1);
	bar1 = pci_iomap(pdev, 1, size);
	if (!bar1) { log_line("iomap BAR1 FAILED\n"); goto out_release; }

	/* Allocate coherent DMA buffers */
	desc_buf = dma_alloc_coherent(&pdev->dev, DESC_SIZE, &dma_handle_desc, GFP_KERNEL);
	cfg_buf = dma_alloc_coherent(&pdev->dev, CFG_SIZE, &dma_handle_cfg, GFP_KERNEL);
	if (!desc_buf || !cfg_buf) {
		log_line("DMA alloc FAILED\n");
		goto done;
	}

	memset(desc_buf, 0, DESC_SIZE);
	memset(cfg_buf, 0, CFG_SIZE);

	log_line("Desc buffer: va=%px pa=0x%llx\n", desc_buf, (unsigned long long)dma_handle_desc);
	log_line("Cfg buffer:  va=%px pa=0x%llx\n", cfg_buf, (unsigned long long)dma_handle_cfg);

	/* ================================================================
	 * Step 1: Write interrupt masks and config (from disasm 0x213c-0x2154)
	 * ================================================================ */
	log_line("\n--- Step 1: Interrupt setup ---\n");
	writel(0xfffffe, bar1 + 0x90);
	writel(0xfffffe, bar1 + 0x1090);
	writel(0x3, bar1 + 0x2010);

	u32 verify_90 = readl(bar1 + 0x90);
	u32 verify_1090 = readl(bar1 + 0x1090);
	u32 verify_2010 = readl(bar1 + 0x2010);
	log_line("  bar1+0x90   = 0x%08x (expected 0xfffffe)\n", verify_90);
	log_line("  bar1+0x1090 = 0x%08x (expected 0xfffffe)\n", verify_1090);
	log_line("  bar1+0x2010 = 0x%08x (expected 0x3)\n", verify_2010);

	/* ================================================================
	 * Step 2: Fill descriptor ring with kext's exact pattern (from disasm 0x215e-0x2192)
	 * ================================================================ */
	log_line("\n--- Step 2: Fill desc ring ---\n");
	
	u32 *desc32 = (u32 *)desc_buf;
	desc32[0x40/4] = 0x2d002a80;
	desc32[0x48/4] = 0x2d002a80;
	desc32[0x4c/4] = 0x2a802800;
	desc32[0x50/4] = 0x2d002a80;
	desc32[0x54/4] = 0xbcd3ba4d; /* "MBAK" */
	desc32[0x58/4] = 0x2f802d00;
	desc32[0x60/4] = 0x2f802d00;
	desc32[0x64/4] = 0x2d002a80;
	desc32[0x68/4] = 0x2f802d00;
	desc32[0x6c/4] = 0xbf58bcd3; /* "DCKB" */

	log_line("  Filled desc ring with kext pattern:\n");
	for (int i = 0x40/4; i <= 0x6c/4; i++)
		log_line("    [+0x%02x] = 0x%08x\n", i*4, desc32[i]);

	/* ================================================================
	 * Step 3: Fill secondary config buffer (from disasm 0x21a4-0x21b7)
	 * ================================================================ */
	log_line("\n--- Step 3: Fill cfg buffer ---\n");
	
	u32 *cfg32 = (u32 *)cfg_buf;
	cfg32[0x2c/4] = 0x0;
	cfg32[0x30/4] = 0x6;
	cfg32[0x34/4] = 0x0;

	log_line("  Filled cfg buffer:\n");
	for (int i = 0x2c/4; i <= 0x34/4; i++)
		log_line("    [+0x%02x] = 0x%08x\n", i*4, cfg32[i]);

	/* ================================================================
	 * Step 4: Write desc ring PA to BAR1+0x3080/0x3084 (inferred from Phase 3e)
	 * ================================================================ */
	log_line("\n--- Step 4: Write desc PA ---\n");
	
	u32 pa_lo = (u32)(dma_handle_desc & 0xffffffff);
	u32 pa_hi = (u32)((dma_handle_desc >> 32) & 0xffffffff);

	writel(pa_lo, bar1 + 0x3080);
	writel(pa_hi, bar1 + 0x3084);

	u32 verify_3080 = readl(bar1 + 0x3080);
	u32 verify_3084 = readl(bar1 + 0x3084);
	log_line("  Wrote desc PA: lo=0x%08x hi=0x%08x\n", pa_lo, pa_hi);
	log_line("  Read back:     lo=0x%08x hi=0x%08x (%s)\n",
	       verify_3080, verify_3084,
	       (verify_3080 == pa_lo && verify_3084 == pa_hi) ? "MATCH" : "MISMATCH");

	/* ================================================================
	 * Step 5: Ring doorbell (from disasm 0x21ba)
	 * ================================================================ */
	log_line("\n--- Step 5: Ring doorbell ---\n");
	writel(0x16, bar1 + 0x2004);
	u32 verify_doorbell = readl(bar1 + 0x2004);
	log_line("  Doorbell written (0x16), read back: 0x%08x\n", verify_doorbell);

	/* ================================================================
	 * Step 6: Wait and check for activity
	 * ================================================================ */
	mdelay(500);

	log_line("\n--- State AFTER init sequence (500ms wait) ---\n");
	u32 after_3080 = readl(bar1 + 0x3080);
	u32 after_doorbell = readl(bar1 + 0x2004);
	u32 ch0_status = readl(bar1 + 0x104c);
	u32 eng0 = readl(bar1 + 0x3000);

	log_line("  bar1+0x3080 (desc_lo) = 0x%08x\n", after_3080);
	log_line("  bar1+0x2004 (doorbell) = 0x%08x\n", after_doorbell);
	log_line("  ch0_status (+0x104c)   = 0x%08x\n", ch0_status);
	log_line("  eng0 (+0x3000)         = 0x%08x\n", eng0);

	/* Check if card READ from our desc buffer (pattern changed?) */
	log_line("\n--- Desc buffer check ---\n");
	int changes = 0;
	for (int i = 0; i < DESC_SIZE/4; i++) {
		u32 expected = (i >= 0x40/4 && i <= 0x6c/4) ? desc32[i] : 0;
		if (desc32[i] != expected) {
			if (changes == 0)
				log_line("  First change at offset %d: was 0x%08x, now 0x%08x\n", i*4, expected, desc32[i]);
			changes++;
		}
	}

	if (changes > 0) {
		log_line("  *** CARD MODIFIED OUR DESC BUFFER! %d dwords changed ***\n", changes);
		log_line("  First 64 bytes after:\n");
		for (int i = 0; i < 16; i++)
			log_line("    [%02x] %08x\n", i*4, desc32[i]);
	} else {
		log_line("  No changes in desc buffer (card did NOT modify it)\n");
	}

	/* Check channel status words */
	log_line("\n--- Channel status scan ---\n");
	for (int ch = 0; ch < 16; ch++) {
		u32 off = 0x1000 + ch * 0x100 + 0x4c;
		if (off >= size) break;
		u32 v = readl(bar1 + off);
		if (v != 0 && v != 0xffffffff)
			log_line("  ch%d status (bar1+0x%04x) = 0x%08x\n", ch, off, v);
	}

	dma_free_coherent(&pdev->dev, DESC_SIZE, desc_buf, dma_handle_desc);
	dma_free_coherent(&pdev->dev, CFG_SIZE, cfg_buf, dma_handle_cfg);

done:
	log_line("\n=== Phase 3g complete ===\n");
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
	.name	= "afterburner_init_seq",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 3g replicate kext init sequence");
