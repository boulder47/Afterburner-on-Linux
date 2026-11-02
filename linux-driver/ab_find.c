// SPDX-License-Identifier: GPL-2.0
/* List which devices the kernel PCI core can see (vs raw lspci). */
#include <linux/module.h>
#include <linux/pci.h>

static int __init ab_find_init(void)
{
	struct pci_dev *d;

	pr_info("ab_find: scanning all devices\n");
	for_each_pci_dev(d) {
		if (d->bus && d->bus->number >= 0x0a && d->bus->number <= 0x10)
			pr_info("ab_find:   %s devfn=0x%02x class=%08x\n",
			       pci_name(d), d->devfn, d->class);
	}
	return 0;
}

static void __exit ab_find_exit(void) {}

module_init(ab_find_init);
module_exit(ab_find_exit);
MODULE_LICENSE("GPL");
