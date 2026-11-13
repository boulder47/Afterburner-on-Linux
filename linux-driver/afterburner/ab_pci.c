// SPDX-License-Identifier: GPL-2.0
/*
 * ab_pci.c — PCI probe/remove/err_handler for the Apple Afterburner [106b:0205].
 *
 * Phase 3u.1 (plan.md §8): SHORT PROBE ONLY — BAR map + decoder bring-up, stop
 * command on remove. All decode work is ioctl-triggered in process context from
 * 3u.2 onward; nothing long-running happens here (§28: a 75 s sequence inside
 * .probe delayed AER recovery ~80 s and killed the card).
 */

#include <linux/pci.h>

#include "include/ab.h"
#include "include/ab_regs.h"

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x0205

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct ab_device *ab;
	int ret;

	ab = devm_kzalloc(&pdev->dev, sizeof(*ab), GFP_KERNEL);
	if (!ab)
		return -ENOMEM;
	ab->pdev = pdev;
	mutex_init(&ab->dma.lock);

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "pci_enable_device: %d\n", ret);
		return ret;
	}
	pci_set_master(pdev);

	ret = pci_request_regions(pdev, "afterburner");
	if (ret)
		goto out_disable;

	ab->bar0 = pci_iomap(pdev, 0, 0);	/* full BAR0 (128K) */
	ab->bar1 = pci_iomap(pdev, 1, pci_resource_len(pdev, 1));
	if (!ab->bar0 || !ab->bar1) {
		dev_err(&pdev->dev, "BAR ioremap failed\n");
		ret = -ENOMEM;
		goto out_unmap;
	}

	dev_info(&pdev->dev, "Apple Afterburner [106b:0205] bound (BAR0=128K BAR1=64K)\n");

	ret = ab_bringup(ab);
	if (ret) {
		dev_err(&pdev->dev, "decoder bring-up failed (%d) — not binding\n", ret);
		goto out_unmap;
	}

	pci_set_drvdata(pdev, ab);

	/* 3u.2 — /dev/afterburner0 (ioctl-triggered decode; short-probe design) */
	ret = ab_chardev_register(ab);
	if (ret) {
		dev_err(&pdev->dev, "chardev registration failed (%d)\n", ret);
		ab_stop(ab);	/* decoder was brought up — stop before unmap */
		goto out_unmap;
	}

	/* Phase A — read-only sysfs surface (identity/engine/thermal) */
	ret = ab_sysfs_register(ab);
	if (ret) {
		dev_err(&pdev->dev, "sysfs registration failed (%d)\n", ret);
		ab_chardev_unregister(ab);
		ab_stop(ab);
		goto out_unmap;
	}
	return 0;

out_unmap:
	if (ab->bar1)
		pci_iounmap(pdev, ab->bar1);
	if (ab->bar0)
		pci_iounmap(pdev, ab->bar0);
	pci_release_regions(pdev);
out_disable:
	pci_disable_device(pdev);
	return ret;
}

static void ab_remove(struct pci_dev *pdev)
{
	struct ab_device *ab = pci_get_drvdata(pdev);

	if (ab) {
		/* Drop /dev node first so no new ioctl can start. */
		ab_chardev_unregister(ab);
		/* sysfs attrs read live registers — drop before iounmap too. */
		ab_sysfs_unregister(ab);
		/* Free persistent chains + bound DMA regions (waits for any
		 * in-flight submit holding dm->lock). Must precede iounmap:
		 * dma_free_coherent may program the device's IOMMU/ATU state. */
		ab_dma_release(ab, true);
		/* kext stop() — decoder disable before we unmap (§8.4 3u.1
		 * acceptance: BAR0[hdr+4]=0 verified after rmmod). */
		ab_stop(ab);
		pci_iounmap(pdev, ab->bar1);
		pci_iounmap(pdev, ab->bar0);
	}
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static pci_ers_result_t ab_error_detected(struct pci_dev *pdev,
					  pci_channel_state_t state)
{
	dev_warn(&pdev->dev, "AER error detected (state=%d); log only — slot-reset recovery not implemented\n",
		 state);
	return PCI_ERS_RESULT_CAN_RECOVER;
}

static const struct pci_error_handlers ab_err_handlers = {
	.error_detected = ab_error_detected,
};

static const struct pci_device_id ab_ids[] = {
	{ PCI_DEVICE(AB_VENDOR, AB_DEVICE) },
	{}
};
MODULE_DEVICE_TABLE(pci, ab_ids);

static struct pci_driver ab_driver = {
	.name = "afterburner",
	.id_table = ab_ids,
	.probe = ab_probe,
	.remove = ab_remove,
	.err_handler = &ab_err_handlers,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("afterburner-re");
MODULE_DESCRIPTION("Apple Afterburner ProRes decoder driver (Phase 3u)");
