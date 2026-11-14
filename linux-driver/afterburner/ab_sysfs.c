// SPDX-License-Identifier: GPL-2.0
/*
 * ab_sysfs.c — read-only sysfs surface under /sys/class/afterburner/afterburnerN
 * (plan2.md Phase A; mirrors the kext's IOKit registry properties, plan.md §2).
 *
 * Every attribute reads live registers on access — no cached state. Identity
 * words come from the BAR0+0x0 header block (§20.1 walk: tag 1 → STATUS slot);
 * thermal words are the per-submit snapshot the kext takes in submitFrame
 * (fn@0xa9ac, byte-verified): base = "main" region BAR0+0x1000 (self->+0x140),
 * offsets +0x10 status / +0x80..+0x8C ADCs (val>>7 °C) / +0x94 sysmon raw.
 */

#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#include "include/ab.h"
#include "include/ab_regs.h"

/* BAR0 header block (section @+0x0, §20.1) */
#define AB_B0_HDR_BOARD_ID   4    /* 0x10020014 — AMD vendor; class=(>>16)&0xF */
#define AB_B0_HDR_FUSE_USER  0xC  /* 0xEC01003F (idle) */

/* Thermal section — relative to BAR0+0x1000 ("main" region, kext self->+0x140).
 * NOTE: Phase 1 misread +0x1080.. as a "desc table"; the submitFrame disassembly
 * proves these are live ADCs (FINDINGS §38.2). */
#define AB_B0_THERMAL_BASE      0x1000
#define AB_B0_THERM_STATUS      (AB_B0_THERMAL_BASE + 0x10)
#define AB_B0_THERM_BACKCARD    (AB_B0_THERMAL_BASE + 0x80)
#define AB_B0_THERM_FPGA_DIE    (AB_B0_THERMAL_BASE + 0x84)
#define AB_B0_THERM_FACEPLATE   (AB_B0_THERMAL_BASE + 0x88)
#define AB_B0_THERM_BOTTOM      (AB_B0_THERMAL_BASE + 0x8C)
#define AB_B0_THERM_SYSMON_RAW  (AB_B0_THERMAL_BASE + 0x94)

static ssize_t ab_show_u32hex(struct device *dev, char *buf, u32 bar_off, int bar)
{
	struct ab_device *ab = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", ab_read(ab, bar, bar_off));
}

static ssize_t board_id_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return ab_show_u32hex(dev, buf, AB_B0_HDR_BOARD_ID, AB_BAR0);
}

/* kext logs the fw word as "%u.%u" (upper16.minor): 0x0002003E -> "2.62" */
static ssize_t fw_version_show(struct device *dev, struct device_attribute *da, char *buf)
{
	struct ab_device *ab = dev_get_drvdata(dev);
	u32 w = ab_read(ab, AB_BAR0, AB_B0_FW_VERSION);

	return sysfs_emit(buf, "%u.%u\n", (w >> 16) & 0xFFFF, w & 0xFFFF);
}

/* board class = (board_rev>>16)&0xF — kext jump table @0x1e84 (§20.2): the word
 * doubles as the booted image type */
static ssize_t image_type_show(struct device *dev, struct device_attribute *da, char *buf)
{
	struct ab_device *ab = dev_get_drvdata(dev);
	u32 cls = (ab_read(ab, AB_BAR0, AB_B0_HDR_BOARD_ID) >> 16) & 0xF;

	switch (cls) {
	case 0: return sysfs_emit(buf, "golden\n");
	case 1: return sysfs_emit(buf, "hwte\n");
	case 2: return sysfs_emit(buf, "live\n");
	case 3: return sysfs_emit(buf, "debug\n");
	default: return sysfs_emit(buf, "unknown(%u)\n", cls);
	}
}

static ssize_t fuse_user_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return ab_show_u32hex(dev, buf, AB_B0_HDR_FUSE_USER, AB_BAR0);
}

static ssize_t decoder_ready_show(struct device *dev, struct device_attribute *da, char *buf)
{
	struct ab_device *ab = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", (ab_read(ab, AB_BAR0, ab->hdr_off + AB_DEC_READY) & 1));
}

static ssize_t decoder_enable_show(struct device *dev, struct device_attribute *da, char *buf)
{
	struct ab_device *ab = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", ab_read(ab, AB_BAR0, ab->hdr_off + AB_DEC_ENABLE));
}

static ssize_t engine_ctrl_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return ab_show_u32hex(dev, buf, AB_B1_ENGINE_CTRL, AB_BAR1);
}

/* raw completion word — bit0 valid, [31:1] count (engine-owned; read-only view) */
static ssize_t completion_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return ab_show_u32hex(dev, buf, AB_B1_COMPLETION, AB_BAR1);
}

/* channel status words — bit0 = valid (engine-owned; wedge diagnostics:
 * h2cStatus +0x44 / c2hStatus +0x1044 show whether DMA is mid-flight when a
 * frame never completes). */
static ssize_t h2c_status_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return ab_show_u32hex(dev, buf, AB_B1_DATA_SEND, AB_BAR1);
}

static ssize_t c2h_status_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return ab_show_u32hex(dev, buf, AB_B1_DATA_BATCH, AB_BAR1);
}

/* dual-ack registers — engine-owned (§3n: host writes don't stick; the engine
 * rewrites them on ack). Exposed for drift checks in the B4 RECOVER soak. */
static ssize_t ack0_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return ab_show_u32hex(dev, buf, AB_B1_ACK_0, AB_BAR1);
}

static ssize_t ack1_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return ab_show_u32hex(dev, buf, AB_B1_ACK_1, AB_BAR1);
}

static ssize_t thermal_status_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return ab_show_u32hex(dev, buf, AB_B0_THERM_STATUS, AB_BAR0);
}

/* ADC fixed-point: °C = val >> 7 (kext log strings "thermal ... ADC: %u C") */
static ssize_t temp_c_show(struct device *dev, char *buf, u32 off)
{
	struct ab_device *ab = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", (int)(ab_read(ab, AB_BAR0, off) >> 7));
}

static ssize_t temp_backcard_c_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return temp_c_show(dev, buf, AB_B0_THERM_BACKCARD);
}

static ssize_t temp_fpga_die_c_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return temp_c_show(dev, buf, AB_B0_THERM_FPGA_DIE);
}

static ssize_t temp_faceplate_c_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return temp_c_show(dev, buf, AB_B0_THERM_FACEPLATE);
}

static ssize_t temp_bottom_c_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return temp_c_show(dev, buf, AB_B0_THERM_BOTTOM);
}

static ssize_t thermal_sysmon_raw_show(struct device *dev, struct device_attribute *da, char *buf)
{
	return ab_show_u32hex(dev, buf, AB_B0_THERM_SYSMON_RAW, AB_BAR0);
}

static struct device_attribute ab_attrs[] = {
	__ATTR_RO(board_id),
	__ATTR_RO(fw_version),
	__ATTR_RO(image_type),
	__ATTR_RO(fuse_user),
	__ATTR_RO(decoder_ready),
	__ATTR_RO(decoder_enable),
	__ATTR_RO(engine_ctrl),
	__ATTR_RO(completion),
	__ATTR_RO(h2c_status),
	__ATTR_RO(c2h_status),
	__ATTR_RO(ack0),
	__ATTR_RO(ack1),
	__ATTR_RO(thermal_status),
	__ATTR_RO(temp_backcard_c),
	__ATTR_RO(temp_fpga_die_c),
	__ATTR_RO(temp_faceplate_c),
	__ATTR_RO(temp_bottom_c),
	__ATTR_RO(thermal_sysmon_raw),
};

int ab_sysfs_register(struct ab_device *ab)
{
	int i, ret = 0;

	if (!ab->devnode) {
		dev_dbg(&ab->pdev->dev, "sysfs: no devnode — skipping\n");
		return 0;
	}
	for (i = 0; i < ARRAY_SIZE(ab_attrs); i++) {
		ret = device_create_file(ab->devnode, &ab_attrs[i]);
		if (ret) {
			dev_err(&ab->pdev->dev, "sysfs: create %s failed (%d)\n",
				ab_attrs[i].attr.name, ret);
			while (--i >= 0)
				device_remove_file(ab->devnode, &ab_attrs[i]);
			return ret;
		}
	}
	dev_info(&ab->pdev->dev, "sysfs: %zu attributes under /sys/class/afterburner/\n",
		 ARRAY_SIZE(ab_attrs));
	return 0;
}

void ab_sysfs_unregister(struct ab_device *ab)
{
	int i;

	if (!ab->devnode)
		return;
	for (i = 0; i < ARRAY_SIZE(ab_attrs); i++)
		device_remove_file(ab->devnode, &ab_attrs[i]);
}
