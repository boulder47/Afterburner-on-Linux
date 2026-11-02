// SPDX-License-Identifier: GPL-2.0
/* D3hot -> D0 power-state cycle on 0f:00.0 (Apple Afterburner).
 * Stronger than FLR/bus reset for firmware that wedges after a PCIe
 * fatal error; weaker than a full power cut. */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>

static int __init ab_d3_init(void)
{
	struct pci_dev *pdev = NULL, *d;
	u16 pmcsr;
	int pm_cap, rc;

	for_each_pci_dev(d) {
		if (d->bus && d->bus->number == 0x0f &&
		    d->devfn == PCI_DEVFN(0, 0))
			pdev = d;
	}
	if (!pdev) {
		pr_info("ab_d3: card not found\n");
		return -ENODEV;
	}

	pm_cap = pci_find_capability(pdev, PCI_CAP_ID_PM);
	if (pm_cap < 0) {
		pr_info("ab_d3: no PM capability (%d)\n", pm_cap);
		goto out;
	}
	pci_read_config_word(pdev, pm_cap + PCI_PM_CTRL, &pmcsr);
	pr_info("ab_d3: pm cap @0x%02x pmcsr=0x%04x\n", pm_cap, pmcsr);

	rc = pci_set_power_state(pdev, PCI_D3hot);
	pr_info("ab_d3: D3hot rc=%d\n", rc);
	msleep(3000);

	rc = pci_set_power_state(pdev, PCI_D0);
	pr_info("ab_d3: D0 rc=%d\n", rc);
	msleep(8000); /* firmware re-init */

	if (!pci_enable_device(pdev)) {
		void __iomem *b1;
		u32 v;

		b1 = pci_iomap(pdev, 1, 0);
		v = b1 ? readl(b1 + 0x0) : 0xDEADDEAD;
		pr_info("ab_d3: post-cycle BAR1+0x0=0x%08x\n", v);
		if (b1)
			pci_iounmap(pdev, b1);
		pci_disable_device(pdev);
	} else {
		pr_info("ab_d3: re-enable failed\n");
	}
out:
	pci_dev_put(pdev);
	return 0;
}

static void __exit ab_d3_exit(void) {}

module_init(ab_d3_init);
module_exit(ab_d3_exit);
MODULE_LICENSE("GPL");
