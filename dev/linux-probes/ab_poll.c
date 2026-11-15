// SPDX-License-Identifier: GPL-2.0
/* Sample BAR1+0x0 (engine ctrl/status) and BAR0+0x8 (fw version).
 * Load/unload repeatedly to poll a wedged card after reset. */
#include <linux/module.h>
#include <linux/pci.h>

static int __init ab_poll_init(void)
{
	struct pci_dev *pdev;
	void __iomem *b0, *b1;
	u32 v0, v1;

	pdev = pci_get_domain_bus_and_slot(0, 0x0f, PCI_DEVFN(0, 0));
	if (!pdev) {
		pr_info("ab_poll: device not found\n");
		return -ENODEV;
	}
	if (pci_enable_device(pdev)) {
		pr_info("ab_poll: enable failed\n");
		pci_dev_put(pdev);
		return -EIO;
	}
	b0 = pci_iomap(pdev, 0, 0);
	b1 = pci_iomap(pdev, 1, 0);
	if (!b0 || !b1) {
		pr_info("ab_poll: iomap failed\n");
		goto out;
	}
	v0 = readl(b0 + 0x8);
	v1 = readl(b1 + 0x0);
	pr_info("ab_poll: BAR0+0x8=0x%08x BAR1+0x0=0x%08x\n", v0, v1);
	pci_iounmap(pdev, b1);
	pci_iounmap(pdev, b0);
out:
	pci_disable_device(pdev);
	pci_dev_put(pdev);
	return 0;
}

static void __exit ab_poll_exit(void) {}

module_init(ab_poll_init);
module_exit(ab_poll_exit);
MODULE_LICENSE("GPL");
