// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_bar1_test.c — Phase 3e: verify doorbell at BAR1+0x2004 + hunt for desc-ring PA register.
 *
 * Part A: Doorbell verification
 *   - Read BAR1+0x2004 (should be 0 or latched value)
 *   - Write a test value (0x16, same as init)
 *   - Read back immediately (doorbells often clear to 0 after write)
 *   - Wait, read again
 *   - Check if any BAR1 state changed (channel status words, engine blocks)
 *
 * Part B: Desc-ring PA register hunt
 *   - Allocate a small DMA-coherent buffer (64KB — well within pool limits)
 *   - Get its physical address
 *   - Sweep candidate BAR1 offsets with the PA write + read-back test
 *   - Candidates: low offsets (+0x0..+0xff), channel bases, engine blocks, and
 *     the specific offsets from configureInterrupts context
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

static const char *out_path = "/tmp/bar1_test.txt";

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
	void *dma_buf;
	const u32 DMA_BUF_SIZE = 65536; /* 64KB — safe for coherent pool */

	log_line("=== Phase 3e: BAR1 doorbell verify + desc-PA hunt ===\n\n");

	if (pci_enable_device(pdev)) { log_line("pci_enable_device FAILED\n"); return -ENODEV; }
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_bar1_test")) { log_line("request_regions FAILED\n"); goto out_disable; }

	size = pci_resource_len(pdev, 1);
	bar1 = pci_iomap(pdev, 1, size);
	if (!bar1) { log_line("iomap BAR1 FAILED\n"); goto out_release; }

	/* ================================================================
	 * PART A: Doorbell verification at BAR1+0x2004
	 * ================================================================ */
	log_line("--- PART A: Doorbell test (BAR1+0x2004) ---\n");

	u32 before_2004 = readl(bar1 + 0x2004);
	u32 before_ch0_status = readl(bar1 + 0x104c);   /* channel 0 status */
	u32 before_eng0 = readl(bar1 + 0x3000);         /* engine block 0 base */

	log_line("  Before: bar1+0x2004=0x%08x ch0_status(+0x104c)=0x%08x eng0(+0x3000)=0x%08x\n",
	       before_2004, before_ch0_status, before_eng0);

	/* Write the init doorbell value */
	writel(0x16, bar1 + 0x2004);
	u32 after_write = readl(bar1 + 0x2004);
	log_line("  After write(0x16): bar1+0x2004=0x%08x (cleared=%s)\n",
	       after_write, (after_write == 0) ? "YES" : "NO");

	/* Wait a bit and re-read */
	mdelay(50);
	u32 after_wait = readl(bar1 + 0x2004);
	log_line("  After 50ms wait: bar1+0x2004=0x%08x\n", after_wait);

	/* Check if state changed */
	u32 after_ch0_status = readl(bar1 + 0x104c);
	u32 after_eng0 = readl(bar1 + 0x3000);
	log_line("  After: ch0_status=0x%08x (changed=%s) eng0=0x%08x (changed=%s)\n",
	       after_ch0_status, (after_ch0_status != before_ch0_status) ? "YES" : "no",
	       after_eng0, (after_eng0 != before_eng0) ? "YES" : "no");

	/* Try a different doorbell value to see if it's value-sensitive */
	writel(0x01, bar1 + 0x2004);
	mdelay(50);
	u32 after_01 = readl(bar1 + 0x2004);
	log_line("  After write(0x01): bar1+0x2004=0x%08x\n", after_01);

	/* Restore to init value */
	writel(0x16, bar1 + 0x2004);
	mdelay(10);

	log_line("\n");

	/* ================================================================
	 * PART B: Desc-ring PA register hunt
	 * ================================================================ */
	log_line("--- PART B: Desc-PA register sweep ---\n");

	dma_buf = dma_alloc_coherent(&pdev->dev, DMA_BUF_SIZE, &dma_handle, GFP_KERNEL);
	if (!dma_buf) {
		log_line("  dma_alloc_coherent(64KB) FAILED — cannot test PA writes\n");
		goto done;
	}
	log_line("  DMA buffer: va=%px pa=0x%llx size=%u\n", dma_buf, (unsigned long long)dma_handle, DMA_BUF_SIZE);

	u32 pa_lo = (u32)(dma_handle & 0xffffffff);
	u32 pa_hi = (u32)((dma_handle >> 32) & 0xffffffff);
	log_line("  PA: lo=0x%08x hi=0x%08x\n", pa_lo, pa_hi);

	/* Candidate offsets to test for desc-PA acceptance.
	 * Strategy: test pairs of consecutive dwords (lo/hi) at various locations. */
	struct { u32 off; const char *label; } candidates[] = {
		{ 0x000, "header+0x0" },
		{ 0x010, "header+0x10" },
		{ 0x020, "header+0x20" },
		{ 0x030, "header+0x30" },
		{ 0x040, "header+0x40" },
		{ 0x050, "header+0x50" },
		{ 0x060, "header+0x60" },
		{ 0x070, "header+0x70" },
		{ 0x100, "ch0 region+0x100" },
		{ 0x110, "ch0 region+0x110" },
		{ 0x120, "ch0 region+0x120" },
		{ 0x130, "ch0 region+0x130" },
		{ 0x140, "ch0 region+0x140" },
		{ 0x200, "ch1 region+0x200" },
		{ 0x300, "eng0 region+0x300" },
		{ 0x310, "eng0 region+0x310" },
		{ 0x400, "+0x400" },
		{ 0x500, "+0x500" },
		{ 0x600, "+0x600" },
		{ 0x700, "+0x700" },
		{ 0x800, "+0x800" },
		{ 0x900, "+0x900" },
		{ 0xa00, "+0xa00" },
		{ 0xb00, "+0xb00" },
		{ 0xc00, "+0xc00" },
		{ 0xd00, "+0xd00" },
		{ 0xe00, "+0xe00" },
		{ 0xf00, "+0xf00" },
	};

	int found = 0;
	for (int i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
		u32 off = candidates[i].off;
		if (off + 8 > size) continue;

		/* Read current values */
		u32 orig_lo = readl(bar1 + off);
		u32 orig_hi = readl(bar1 + off + 4);

		/* Write PA */
		writel(pa_lo, bar1 + off);
		writel(pa_hi, bar1 + off + 4);

		/* Read back */
		u32 rb_lo = readl(bar1 + off);
		u32 rb_hi = readl(bar1 + off + 4);

		if (rb_lo == pa_lo && rb_hi == pa_hi) {
			log_line("  *** MATCH at bar1+0x%04x (%s): lo=0x%08x hi=0x%08x ACCEPTED ***\n",
			       off, candidates[i].label, rb_lo, rb_hi);
			found++;
			/* Restore original values */
			writel(orig_lo, bar1 + off);
			writel(orig_hi, bar1 + off + 4);
		} else if (rb_lo != orig_lo || rb_hi != orig_hi) {
			log_line("  ~~~ CHANGED at bar1+0x%04x (%s): was lo=0x%08x hi=0x%08x, now lo=0x%08x hi=0x%08x (partial accept?)\n",
			       off, candidates[i].label, orig_lo, orig_hi, rb_lo, rb_hi);
			/* Restore */
			writel(orig_lo, bar1 + off);
			writel(orig_hi, bar1 + off + 4);
		}
	}

	if (found == 0) {
		log_line("  No exact PA matches found in candidate sweep.\n");
		log_line("  Trying FULL BAR1 sweep: every 0x20 bytes from 0x0 to 0xffff...\n");
		for (u32 off = 0; off < size && found == 0; off += 0x20) {
			u32 orig_lo = readl(bar1 + off);
			u32 orig_hi = readl(bar1 + off + 4);

			writel(pa_lo, bar1 + off);
			writel(pa_hi, bar1 + off + 4);

			u32 rb_lo = readl(bar1 + off);
			u32 rb_hi = readl(bar1 + off + 4);

			if (rb_lo == pa_lo && rb_hi == pa_hi) {
				log_line("  *** FULL MATCH at bar1+0x%04x: PA ACCEPTED ***\n", off);
				found++;
				writel(orig_lo, bar1 + off);
				writel(orig_hi, bar1 + off + 4);
			} else if (rb_lo != orig_lo || rb_hi != orig_hi) {
				log_line("  ~~~ FULL CHANGED at bar1+0x%04x: was 0x%08x/0x%08x, now 0x%08x/0x%08x\n",
				       off, orig_lo, orig_hi, rb_lo, rb_hi);
				writel(orig_lo, bar1 + off);
				writel(orig_hi, bar1 + off + 4);
			}
		}
		if (found == 0) log_line("  Full BAR1 sweep: no matches in entire 64KB.\n");
	}

	/* ================================================================
	 * PART C: Probe for ack/status registers near the doorbell
	 * ================================================================ */
	log_line("\n--- PART C: Doorbell neighborhood probe ---\n");
	log_line("  Scanning BAR1+0x2000..0x2040 for status/ack regs after doorbell write:\n");

	/* Write doorbell, then scan nearby offsets for any that change */
	u32 before[16];
	for (int i = 0; i < 16; i++)
		before[i] = readl(bar1 + 0x2000 + i * 4);

	writel(0x16, bar1 + 0x2004);
	mdelay(100);

	for (int i = 0; i < 16; i++) {
		u32 after = readl(bar1 + 0x2000 + i * 4);
		if (after != before[i])
			log_line("  *** bar1+0x%04x CHANGED: 0x%08x -> 0x%08x ***\n",
			       0x2000 + i * 4, before[i], after);
	}

	/* Also check channel status words (each channel has a status at base+0x4c) */
	log_line("  Channel status scan after doorbell:\n");
	for (int ch = 0; ch < 16; ch++) {
		u32 off = 0x1000 + ch * 0x100 + 0x4c; /* channel base + 0x4c */
		if (off >= size) break;
		u32 v = readl(bar1 + off);
		if (v != 0 && v != 0xffffffff)
			log_line("    ch%d status (bar1+0x%04x) = 0x%08x\n", ch, off, v);
	}

	/* Check engine blocks for any activity */
	log_line("  Engine block scan after doorbell:\n");
	for (u32 off = 0x3000; off < 0x4000; off += 0x100) {
		u32 v = readl(bar1 + off);
		if (v != 0 && v != 0xffffffff)
			log_line("    eng (bar1+0x%04x) = 0x%08x\n", off, v);
	}

	dma_free_coherent(&pdev->dev, DMA_BUF_SIZE, dma_buf, dma_handle);

done:
	log_line("\n=== Phase 3e complete ===\n");
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
	.name	= "afterburner_bar1_test",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 3e BAR1 doorbell verify + desc-PA hunt");
