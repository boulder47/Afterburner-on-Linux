// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_dma_test.c — Phase 3f: First real DMA submit test.
 *
 * Strategy:
 *   1. Allocate a small coherent DMA buffer (4KB) filled with a known pattern
 *   2. Write its PA to BAR1+0x3080/0x3084 (desc-ring base)
 *   3. Ring the doorbell at BAR1+0x2004
 *   4. Wait, then check:
 *      a) Did any status register change?
 *      b) Did the card READ from our buffer? (pattern changed?)
 *      c) Did the card WRITE to our buffer? (new data appeared?)
 *
 * This tests whether the DMA path works end-to-end, even if we don't yet
 * have a valid ProRes descriptor. If the card fetches our ring, we'll see
 * either a status change or DMA read activity on our buffer.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/slab.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x205

static const char *out_path = "/tmp/dma_test.txt";

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
	dma_addr_t dma_handle;
	u8 *dma_buf;
	const u32 BUF_SIZE = 4096; /* 4KB — small enough for coherent pool */

	log_line("=== Phase 3f: First real DMA submit test ===\n\n");

	if (pci_enable_device(pdev)) { log_line("pci_enable_device FAILED\n"); return -ENODEV; }
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_dma_test")) { log_line("request_regions FAILED\n"); goto out_disable; }

	size = pci_resource_len(pdev, 1);
	bar1 = pci_iomap(pdev, 1, size);
	if (!bar1) { log_line("iomap BAR1 FAILED\n"); goto out_release; }

	/* Allocate coherent DMA buffer with known pattern */
	dma_buf = dma_alloc_coherent(&pdev->dev, BUF_SIZE, &dma_handle, GFP_KERNEL);
	if (!dma_buf) {
		log_line("dma_alloc_coherent(4KB) FAILED\n");
		goto done;
	}

	/* Fill with a recognizable pattern: 0xA5 repeated */
	memset(dma_buf, 0xA5, BUF_SIZE);
	log_line("DMA buffer allocated: va=%px pa=0x%llx size=%u (filled with 0xA5)\n",
	       dma_buf, (unsigned long long)dma_handle, BUF_SIZE);

	/* Save original pattern for later comparison */
	u8 orig_pattern[64];
	memcpy(orig_pattern, dma_buf, sizeof(orig_pattern));

	/* ================================================================
	 * Step 1: Record state BEFORE submit
	 * ================================================================ */
	log_line("\n--- State BEFORE submit ---\n");
	u32 before_3080 = readl(bar1 + 0x3080);
	u32 before_3084 = readl(bar1 + 0x3084);
	u32 before_doorbell = readl(bar1 + 0x2004);
	u32 before_ch0_status = readl(bar1 + 0x104c);
	u32 before_eng0 = readl(bar1 + 0x3000);

	log_line("  bar1+0x3080 (desc_lo) = 0x%08x\n", before_3080);
	log_line("  bar1+0x3084 (desc_hi) = 0x%08x\n", before_3084);
	log_line("  bar1+0x2004 (doorbell) = 0x%08x\n", before_doorbell);
	log_line("  ch0_status (+0x104c)   = 0x%08x\n", before_ch0_status);
	log_line("  eng0 (+0x3000)         = 0x%08x\n", before_eng0);

	/* ================================================================
	 * Step 2: Write desc-ring PA and ring doorbell
	 * ================================================================ */
	log_line("\n--- Submitting ---\n");

	u32 pa_lo = (u32)(dma_handle & 0xffffffff);
	u32 pa_hi = (u32)((dma_handle >> 32) & 0xffffffff);

	writel(pa_lo, bar1 + 0x3080);
	writel(pa_hi, bar1 + 0x3084);

	/* Verify write */
	u32 verify_3080 = readl(bar1 + 0x3080);
	u32 verify_3084 = readl(bar1 + 0x3084);
	log_line("  Wrote desc PA: lo=0x%08x hi=0x%08x\n", pa_lo, pa_hi);
	log_line("  Read back:     lo=0x%08x hi=0x%08x (%s)\n",
	       verify_3080, verify_3084,
	       (verify_3080 == pa_lo && verify_3084 == pa_hi) ? "MATCH" : "MISMATCH");

	/* Ring the doorbell */
	writel(0x16, bar1 + 0x2004);
	log_line("  Doorbell rung (wrote 0x16 to bar1+0x2004)\n");

	/* ================================================================
	 * Step 3: Wait and check for activity
	 * ================================================================ */
	mdelay(500); /* Wait 500ms for card to process */

	log_line("\n--- State AFTER submit (500ms wait) ---\n");
	u32 after_3080 = readl(bar1 + 0x3080);
	u32 after_3084 = readl(bar1 + 0x3084);
	u32 after_doorbell = readl(bar1 + 0x2004);
	u32 after_ch0_status = readl(bar1 + 0x104c);
	u32 after_eng0 = readl(bar1 + 0x3000);

	log_line("  bar1+0x3080 (desc_lo) = 0x%08x (%s)\n", after_3080,
	       (after_3080 != before_3080) ? "CHANGED" : "same");
	log_line("  bar1+0x3084 (desc_hi) = 0x%08x (%s)\n", after_3084,
	       (after_3084 != before_3084) ? "CHANGED" : "same");
	log_line("  bar1+0x2004 (doorbell) = 0x%08x (%s)\n", after_doorbell,
	       (after_doorbell != before_doorbell) ? "CHANGED" : "same");
	log_line("  ch0_status (+0x104c)   = 0x%08x (%s)\n", after_ch0_status,
	       (after_ch0_status != before_ch0_status) ? "CHANGED" : "same");
	log_line("  eng0 (+0x3000)         = 0x%08x (%s)\n", after_eng0,
	       (after_eng0 != before_eng0) ? "CHANGED" : "same");

	/* Check if card READ from our buffer (pattern changed?) */
	log_line("\n--- DMA buffer check ---\n");
	int reads_detected = 0;
	for (int i = 0; i < BUF_SIZE; i++) {
		if (dma_buf[i] != 0xA5) {
			if (reads_detected == 0)
				log_line("  First change at offset %d: was 0xA5, now 0x%02x\n", i, dma_buf[i]);
			reads_detected++;
		}
	}

	if (reads_detected > 0) {
		log_line("  *** CARD READ/WROTE TO OUR BUFFER! %d bytes changed ***\n", reads_detected);
		log_line("  First 64 bytes after:\n");
		for (int i = 0; i < 64; i++)
			log_line("    [%02x] %02x%s", i, dma_buf[i], (i % 16 == 15) ? "\n" : " ");
		if ((64 % 16) != 0) log_line("\n");
	} else {
		log_line("  No changes detected in DMA buffer (card did NOT read/write to it)\n");
	}

	/* Check channel status words for any activity */
	log_line("\n--- Channel status scan ---\n");
	for (int ch = 0; ch < 16; ch++) {
		u32 off = 0x1000 + ch * 0x100 + 0x4c;
		if (off >= size) break;
		u32 v = readl(bar1 + off);
		if (v != 0 && v != 0xffffffff)
			log_line("  ch%d status (bar1+0x%04x) = 0x%08x\n", ch, off, v);
	}

	/* Check engine blocks */
	log_line("\n--- Engine block scan ---\n");
	for (u32 off = 0x3000; off < 0x4000; off += 0x100) {
		u32 v = readl(bar1 + off);
		if (v != 0 && v != 0xffffffff)
			log_line("  eng (bar1+0x%04x) = 0x%08x\n", off, v);
	}

	dma_free_coherent(&pdev->dev, BUF_SIZE, dma_buf, dma_handle);

done:
	log_line("\n=== Phase 3f complete ===\n");
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
	.name	= "afterburner_dma_test",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 3f first real DMA submit test");
