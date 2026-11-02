// SPDX-License-Identifier: GPL-2.0
/* One-shot Function Level Reset on 0f:00.0 (Apple Afterburner).
 * Used when the card wedges after a PCIe fatal error and sysfs reset
 * is not available without a password. */
#include <linux/module.h>
#include <linux/pci.h>

static int __init ab_reset_init(void)
{
	struct pci_dev *pdev;
	int rc;

	pdev = pci_get_domain_bus_and_slot(0, 0x0f, PCI_DEVFN(0, 0));
	if (!pdev) {
		pr_info("ab_reset: device not found\n");
		return -ENODEV;
	}
	rc = pci_reset_function(pdev);
	pr_info("ab_reset: FLR on %s rc=%d\n", pci_name(pdev), rc);
	pci_dev_put(pdev);
	return 0; /* always load OK so the result is readable in dmesg */
}

static void __exit ab_reset_exit(void) {}

module_init(ab_reset_init);
module_exit(ab_reset_exit);
MODULE_LICENSE("GPL");
