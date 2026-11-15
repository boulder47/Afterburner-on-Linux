// SPDX-License-Identifier: GPL-2.0
/* Secondary bus reset on pcieport 0c:10.0 — its only downstream device
 * is the Apple Afterburner at 0f:00.0 (secondary=subordinate=0f).
 * Used when FLR alone fails to revive a wedged card. */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>

static int __init ab_busreset_init(void)
{
	struct pci_dev *port = NULL, *pdev = NULL, *d;
	int rc;

	for_each_pci_dev(d) {
		if (d->bus && d->bus->number == 0x0c &&
		    d->devfn == PCI_DEVFN(0x10, 0))
			port = d; /* ref held by for_each */
	}
	if (!port) {
		pr_info("ab_busreset: port 0c:10.0 not found\n");
		return -ENODEV;
	}
	rc = pci_bridge_secondary_bus_reset(port);
	pr_info("ab_busreset: secondary bus reset on %s rc=%d\n",
	       pci_name(port), rc);

	msleep(5000); /* let the card firmware re-init */

	for_each_pci_dev(d) {
		if (d->bus && d->bus->number == 0x0f &&
		    d->devfn == PCI_DEVFN(0, 0))
			pdev = d;
	}
	if (pdev) {
		void __iomem *b1;
		u32 v;

		if (!pci_enable_device(pdev)) {
			b1 = pci_iomap(pdev, 1, 0);
			v = b1 ? readl(b1 + 0x0) : 0xDEADDEAD;
			pr_info("ab_busreset: post-reset BAR1+0x0=0x%08x\n", v);
			if (b1)
				pci_iounmap(pdev, b1);
			pci_disable_device(pdev);
		} else {
			pr_info("ab_busreset: re-enable failed\n");
		}
	} else {
		pr_info("ab_busreset: card 0f:00.0 vanished from PCI list\n");
	}
	if (port)
		pci_dev_put(port);
	if (pdev)
		pci_dev_put(pdev);
	return 0;
}

static void __exit ab_busreset_exit(void) {}

module_init(ab_busreset_init);
module_exit(ab_busreset_exit);
MODULE_LICENSE("GPL");
