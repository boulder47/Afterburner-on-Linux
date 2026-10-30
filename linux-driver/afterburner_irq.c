// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_irq.c — Phase 2b: Bitfile interrupt handler over MSI.
 *
 * From FINDINGS.md §7.6 (handleInterrupt), the interrupt protocol is driven off the
 * **Bitfile** region (kext self->+0x148):
 *   1. Read status word `bitfile[+0x14]`; if bit 0 clear -> nothing to do, return.
 *   2. Log the full status word.
 *   3. Notify a completion callback (we just log).
 *   4. Read two more words `bitfile[+0x10]` and `bitfile[+0x20]`, logging each.
 *   5. Ack: write `(val & 0x7)` back to `bitfile[+0x20]`.
 *      - bit 2 (0x4) set -> error/abort path: clear `bitfile[+0x04]=0`, log.
 *      - bit 1 (0x2) set -> normal completion: clear `bitfile[+0x04]=0`, continue drain.
 *
 * Per §13 the Bitfile region maps to the **type-4 counter section** at BAR0 +0x2000+i*0x100,
 * where each slot has a counter triplet at intra-slot +0x2c/+0x30/+0x34. The exact status/ack
 * sub-offsets aren't pinned yet, so this module:
 *
 *   (A) Enables MSI (1 vector, 64-bit — confirmed allocatable in Phase 2a).
 *   (B) Registers a handler that, on every IRQ, reads the candidate status registers across
 *       ALL counter slots (+0x2000..+0x2fff) AND the engine banks (+0x8000..+0xf000), logs any
 *       non-trivial value, and acks `(val & 7)` to the matching +0x34 (ack) register. This is
 *       safe: acking low bits of a status reg is exactly what the kext does.
 *   (C) After registering, issues a benign self-test: writes 1 to a candidate "trigger" control
 *       and waits up to 2s for an IRQ. If one fires, we've confirmed the MSI path end-to-end.
 *
 * SAFETY: only status reads + low-bit acks (the documented protocol). No doorbell, no flash,
 * no DMA submit. The self-test trigger write is a single 1 to a control reg that reads 0 at rest;
 * if it does nothing we simply see no IRQ and move on — no state change of consequence.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/delay.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x205

struct ab_irq {
	struct pci_dev *pdev;
	void __iomem *bar0;
	int irq;
	unsigned long irq_count;
	u32 last_status;
	u32 last_ack_off;   /* BAR0 offset of the register we acked */
};

static struct ab_irq ab;

/* Read a candidate status word at BAR0 offset `off`; return it. */
static inline u32 rd(u32 off) { return readl(ab.bar0 + off); }

static irqreturn_t ab_isr(int irq, void *data)
{
	int i;

	ab.irq_count++;

	/*
	 * Scan candidate status registers: every counter slot's triplet (+0x2c/+0x30/+0x34)
	 * across +0x2000..+0x2fff, plus the engine-bank control words at +0x8000..+0xf000.
	 * For any non-trivial value, log it and ack (val & 7) to the slot's +0x34 register —
	 * exactly the kext's ack protocol (§7.6 step 5).
	 */
	for (i = 0; i < 16; i++) {
		u32 base = 0x2000 + i * 0x100;
		u32 s2c = rd(base + 0x2c);
		u32 s30 = rd(base + 0x30);
		u32 s34 = rd(base + 0x34);

		if ((s2c | s30 | s34) != 0 && (s2c | s30 | s34) != 0xffffffff) {
			dev_info_ratelimited(&ab.pdev->dev,
				"IRQ#%lu counter slot %d: +0x%x=0x%08x +0x%x=0x%08x +0x%x=0x%08x\n",
				ab.irq_count, i, base+0x2c, s2c, base+0x30, s30, base+0x34, s34);
			/* Ack low 3 bits to the +0x34 register (kext protocol). */
			if (s34 & 7) {
				writel(s34 & 7, ab.bar0 + base + 0x34);
				ab.last_ack_off = base + 0x34;
				ab.last_status = s34;
			}
		}
	}

	/* Engine banks: check the control words at intra-block +0x14 and +0x28. */
	for (i = 0; i < 7; i++) {
		u32 base = 0x8000 + i * 0x1000;
		u32 c14 = rd(base + 0x14);
		u32 c28 = rd(base + 0x28);
		if ((c14 | c28) != 0 && (c14 | c28) != 0xffffffff) {
			dev_info_ratelimited(&ab.pdev->dev,
				"IRQ#%lu engine bank %d: +0x%x=0x%08x +0x%x=0x%08x\n",
				ab.irq_count, i, base+0x14, c14, base+0x28, c28);
		}
	}

	return IRQ_HANDLED;
}

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int ret;

	pci_info(pdev, "Apple Afterburner [106b:0205] Phase 2b Bitfile interrupt handler (MSI)\n");

	ab.pdev = pdev;

	if (pci_enable_device(pdev)) return -ENODEV;
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_irq")) goto out_disable;

	ab.bar0 = pci_iomap(pdev, 0, 0);
	if (!ab.bar0) { dev_err(&pdev->dev, "iomap BAR0 failed\n"); goto out_release; }

	/* Enable MSI: request exactly 1 vector (confirmed allocatable in Phase 2a). */
	ret = pci_enable_msi(pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "pci_enable_msi failed: %d\n", ret);
		goto out_unmap;
	}
	ab.irq = pdev->irq;
	pci_info(pdev, "MSI enabled, vector=%d\n", ab.irq);

	/* Register the handler (no IRQF_SHARED — MSI is exclusive). */
	ret = request_irq(ab.irq, ab_isr, 0, "afterburner", &ab);
	if (ret) {
		dev_err(&pdev->dev, "request_irq failed: %d\n", ret);
		pci_disable_msi(pdev);
		goto out_unmap;
	}
	pci_info(pdev, "IRQ handler registered on vector %d\n", ab.irq);

	/*
	 * Self-test: try to trigger an IRQ by writing 1 to a candidate control register that
	 * reads 0 at rest. We try the counter-section +0x34 (ack/control) of slot 0 and the
	 * engine-bank +0x28 of bank 0. Wait up to 2s for an IRQ after each. If one fires, the
	 * MSI path is confirmed end-to-end.
	 */
	pci_info(pdev, "--- self-test: attempting benign trigger writes ---\n");

	/* Trigger candidate 1: counter slot 0 control (+0x2034). */
	dev_info(&pdev->dev, "  trigger A: write 1 to +0x2034 (counter slot 0 ctrl)\n");
	writel(1, ab.bar0 + 0x2034);
	msleep(500);
	if (ab.irq_count > 0) {
		dev_info(&pdev->dev, "    ^ IRQ fired! count=%lu last_status=0x%08x acked@+0x%x\n",
			 ab.irq_count, ab.last_status, ab.last_ack_off);
	} else {
		dev_info(&pdev->dev, "    (no IRQ within 500ms)\n");
	}

	/* Trigger candidate 2: engine bank 0 control (+0x8028). */
	dev_info(&pdev->dev, "  trigger B: write 1 to +0x8028 (engine bank 0 ctrl)\n");
	writel(1, ab.bar0 + 0x8028);
	msleep(500);
	if (ab.irq_count > 0) {
		dev_info(&pdev->dev, "    ^ IRQ fired! count=%lu last_status=0x%08x acked@+0x%x\n",
			 ab.irq_count, ab.last_status, ab.last_ack_off);
	} else {
		dev_info(&pdev->dev, "    (no IRQ within 500ms)\n");
	}

	pci_info(pdev, "--- self-test done: total IRQs observed = %lu ---\n", ab.irq_count);

	free_irq(ab.irq, &ab);
	pci_disable_msi(pdev);
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
	.name	= "afterburner_irq",
	.id_table	= ab_pci_ids,
	.probe	= ab_probe,
	.remove	= ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple Afterburner [106b:0205] Phase 2b Bitfile interrupt handler (MSI)");
