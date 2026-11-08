/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _AB_H_
#define _AB_H_

#include <linux/types.h>
#include <linux/pci.h>

struct ab_device {
	struct pci_dev	*pdev;
	void __iomem	*bar0;	/* 128K — control/status regs (FINDINGS §7–§9) */
	void __iomem	*bar1;	/* 64K  — command/doorbell/status surface (§21) */

	u32	hdr_off;	/* decoder section header offset in BAR0 (dir walk) */
	bool	fw_ok;	/* fw version gate passed */
	bool	ready;	/* decoder ready bit set after bring-up */
};

/* ab_regs.c — thin accessors over the two ioremap'd BARs */
u32 ab_read(struct ab_device *ab, int bar, u32 off);
void ab_write(struct ab_device *ab, int bar, u32 off, u32 val);

/* ab_engine.c — decoder bring-up / stop / recovery (kext-exact) */
int  ab_bringup(struct ab_device *ab);
void ab_stop(struct ab_device *ab);
bool ab_recover(struct ab_device *ab);

#endif /* _AB_H_ */
