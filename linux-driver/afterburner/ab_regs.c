// SPDX-License-Identifier: GPL-2.0
/*
 * ab_regs.c — thin readl/writel accessors over the two ioremap'd BARs.
 * Pure port of the register layout in FINDINGS.md §7–§9 / §21; no behavior.
 */

#include <linux/io.h>

#include "include/ab.h"
#include "include/ab_regs.h"

u32 ab_read(struct ab_device *ab, int bar, u32 off)
{
	void __iomem *base = (bar == AB_BAR1) ? ab->bar1 : ab->bar0;

	return readl(base + off);
}

void ab_write(struct ab_device *ab, int bar, u32 off, u32 val)
{
	void __iomem *base = (bar == AB_BAR1) ? ab->bar1 : ab->bar0;

	writel(val, base + off);
}
