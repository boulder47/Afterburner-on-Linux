// SPDX-License-Identifier: GPL-2.0
/*
 * ab_engine.c — decoder bring-up, stop command, recovery.
 *
 * Byte-exact port of the kext (AppleAfterburner arm64e) sequences decoded in
 * FINDINGS.md §21/§25–§30 and UC_METHODS.md:
 *   fw gate       @0x75c4-0x78ac  -> ab_fw_gate()
 *   dir walk      @0x5d14/@0x7918 -> ab_walk_directory()
 *   dec enable    @0x79f8-0x7a64  -> ab_decoder_cycle()
 *   init writer   @0x5a3c-0x5ad0  -> ab_init_writer()
 *   ready poll    ~@0xa700        -> ab_poll_ready()
 *   stop command  fn@0xc0a4       -> ab_stop()
 */

#include <linux/delay.h>
#include <linux/device.h>

#include "include/ab.h"
#include "include/ab_regs.h"

#define AB_DIR_WALK_MAX	16	/* safety cap; live walk is well-formed (stride 0x2C) */
#define AB_READY_POLLS	200	/* x ~10 ms ~= 2 s budget, kext-equivalent */

/* Firmware version gate (@0x75c4-0x78ac): word at BAR0+0x8, upper16 must be 2,
 * lower16 in {0x3E, 0x3F}. Returns true if the kext would proceed. */
static bool ab_fw_gate(struct ab_device *ab)
{
	u32 v = ab_read(ab, AB_BAR0, AB_B0_FW_VERSION);

	dev_info(&ab->pdev->dev, "fw gate: BAR0+0x8=0x%08x (upper=0x%04x lower=0x%04x)\n",
		 v, (v >> 16) & 0xffff, v & 0xffff);

	if (((v >> 16) & 0xffff) != 2 ||
	    ((v & 0xffff) != 0x3e && (v & 0xffff) != 0x3f)) {
		dev_err(&ab->pdev->dev, "fw gate FAIL — kext would stop here\n");
		dev_err(&ab->pdev->dev,
			"diag: BAR0+0x0=0x%08x BAR0+0x8=0x%08x BAR1+0x0=0x%08x (live card: ID hdr w/ 0x1002 vendor; cold BAR1+0x0=0x1FC08006)\n",
			ab_read(ab, AB_BAR0, 0), v, ab_read(ab, AB_BAR1, 0));
		return false;
	}
	return true;
}

/* Walk the section directory (@0x5d14 / copy @0x7918): entry W at offset O:
 * type = W&0xFF (0 terminates), next = O + ((W>>14)&0x3FFFC). Returns the
 * decoder header offset (first type==1) or 0 if not found. On the live card
 * the walk is well-formed: type-1 entry stride=0x2C, terminator at +0x802C. */
static u32 ab_walk_directory(struct ab_device *ab)
{
	u32 off = 0;

	for (int i = 0; i < AB_DIR_WALK_MAX; i++) {
		u32 w = ab_read(ab, AB_BAR0, off);
		u32 type = w & 0xff;
		u32 stride = (w >> 14) & 0x3fffc;

		if (type == 1)
			return off;	/* decoder section header */
		if (type == 0 || stride == 0)
			break;
		off += stride;
	}
	return 0;
}

/* Disable/enable the Decoder Control Register (@0x79f8-0x7a64): hdr+4 = 0,
 * delay ~1 ms (kext bl #0xe89c w0=1), hdr+4 = 1. */
static void ab_decoder_cycle(struct ab_device *ab)
{
	u32 o = ab->hdr_off + AB_DEC_ENABLE;

	ab_write(ab, AB_BAR0, o, 0);
	usleep_range(2000, 5000);
	ab_write(ab, AB_BAR0, o, 1);
}

/* Full init writer @0x5a3c-0x5ad0 — kext-exact (BAR1 words + BAR0 magic region
 * + PR block reset). */
static void ab_init_writer(struct ab_device *ab)
{
	static const u32 magic[10] = {
		0x2D002A80, 0x2D002A80, 0x2A802800, 0x2D002A80, 0xBCD3BA4D,
		0x2F802D00, 0x2F802D00, 0x2D002A80, 0x2F802D00, 0xBF58BCD3,
	};

	ab_write(ab, AB_BAR1, AB_B1_INIT_LO, 0xfffffe);
	ab_write(ab, AB_BAR1, AB_B1_INIT_HI, 0xfffffe);
	ab_write(ab, AB_BAR1, AB_B1_ENGINE_CTRL, 3);

	for (unsigned i = 0; i < ARRAY_SIZE(magic); i++)
		ab_write(ab, AB_BAR0, AB_B0_MAGIC_BASE + i * 4, magic[i]);

	/* the two that DO change on this card (Phase 0: both were 0x3FFF) */
	ab_write(ab, AB_BAR0, AB_B0_MASK_LO, 0x7d);
	ab_write(ab, AB_BAR0, AB_B0_MASK_HI, 0x7d);

	/* PR block reset — +0x202C is NOT writable on this card (readback stays
	 * 7); write-and-ignore per live verification (plan §8.3 item 5). */
	ab_write(ab, AB_BAR0, AB_B0_PR_0, 0);
	ab_write(ab, AB_BAR0, AB_B0_PR_1, 6);
	ab_write(ab, AB_BAR0, AB_B0_PR_2, 0);

	/* engine enable — written exactly once in the whole kext binary */
	ab_write(ab, AB_BAR1, AB_B1_ENGINE_ENABLE, 0x16);
}

/* Poll the readiness bit: frame-submit path @~0xa700 tests bit0 of BAR0[hdr+8]
 * and logs "Decoder not ready!! (0x%08x)" when clear. Up to ~2 s. */
static bool ab_poll_ready(struct ab_device *ab)
{
	u32 r = 0;

	for (int i = 0; i < AB_READY_POLLS; i++) {
		r = ab_read(ab, AB_BAR0, ab->hdr_off + AB_DEC_READY);
		if (r & 1)
			break;
		usleep_range(9000, 11000);
	}

	dev_info(&ab->pdev->dev, "ready: BAR0+0x%x=0x%08x -> %s\n",
		 ab->hdr_off + AB_DEC_READY, r, (r & 1) ? "READY" : "NOT READY after ~2s");
	return r & 1;
}

/* Full decoder bring-up: fw gate -> dir walk -> enable cycle -> init writer ->
 * ready poll. Idempotent (§8.3 item 4): the decoder enable persists across
 * rmmod/insmod, so a warm card may already be ready — the disable/enable cycle
 * is the kext's own recovery path (mode-8 rung A) and converges either way. */
int ab_bringup(struct ab_device *ab)
{
	bool warm;

	if (!ab_fw_gate(ab))
		return -ENODEV;
	ab->fw_ok = true;

	ab->hdr_off = ab_walk_directory(ab);
	if (!ab->hdr_off) {
		dev_err(&ab->pdev->dev, "bring-up: no decoder section in directory\n");
		return -ENXIO;
	}
	dev_info(&ab->pdev->dev, "decoder section header at BAR0+0x%x\n", ab->hdr_off);

	warm = (ab_read(ab, AB_BAR0, ab->hdr_off + AB_DEC_READY) & 1) != 0;

	ab_decoder_cycle(ab);
	ab_init_writer(ab);
	ab->ready = ab_poll_ready(ab);

	dev_info(&ab->pdev->dev, "bring-up %s (warm=%d)\n",
		 ab->ready ? "OK" : "FAILED: decoder not ready", warm);
	return ab->ready ? 0 : -ETIMEDOUT;
}

/* kext stop() fn@0xc0a4 — its ONLY HW write is BAR0[hdr+4]=0 (decoder disable),
 * gated in the kext by self->+0x11d0. */
void ab_stop(struct ab_device *ab)
{
	if (!ab->fw_ok || !ab->hdr_off)
		return;

	ab_write(ab, AB_BAR0, ab->hdr_off + AB_DEC_ENABLE, 0);
	dev_info(&ab->pdev->dev, "stop: BAR0+0x%x=0 (rb 0x%08x)\n",
		 ab->hdr_off + AB_DEC_ENABLE,
		 ab_read(ab, AB_BAR0, ab->hdr_off + AB_DEC_ENABLE));
	ab->ready = false;
}

/* Re-bring-up cycle — rung A of the recovery ladder (§3n): a decoder
 * disable/enable cycle alone un-wedges a stuck engine. FLR fallback lands in
 * 3u.2 with the RECOVER ioctl. */
bool ab_recover(struct ab_device *ab)
{
	if (!ab->fw_ok || !ab->hdr_off)
		return false;

	ab_decoder_cycle(ab);
	ab_init_writer(ab);
	ab->ready = ab_poll_ready(ab);
	return ab->ready;
}

/* FLR hard reset — rung B of the recovery ladder (plan2 Phase B item 4).
 * pci_reset_function() performs a Function Level Reset when the device
 * advertises it in DEVCAP and refuses otherwise (-EOPNOTSUPP) — we never fall
 * back to a secondary-bus reset: this card shares its bus with nothing today,
 * but a link reset would also kill any future co-tenant. FLR wipes engine +
 * decoder state (and restores config space via pci_restore_state inside), so
 * the full bring-up sequence re-runs afterwards; BAR windows stay mapped and
 * host DMA memory survives — userland must re-bind regions after RESET. */
int ab_hard_reset(struct ab_device *ab)
{
	struct ab_dma_state *dm = &ab->dma;
	u32 devcap;
	int ret;

	if (!pci_pcie_cap(ab->pdev)) {
		dev_err(&ab->pdev->dev, "hard reset: not a PCIe device — no FLR\n");
		return -EOPNOTSUPP;
	}
	pci_read_config_dword(ab->pdev, pci_pcie_cap(ab->pdev) + PCI_EXP_DEVCAP, &devcap);
	if (!(devcap & PCI_EXP_DEVCAP_FLR)) {
		dev_err(&ab->pdev->dev, "hard reset: no FLR capability (DEVCAP=0x%08x)\n", devcap);
		return -EOPNOTSUPP;
	}

	mutex_lock(&dm->lock);
	if (dm->in_flight) {
		mutex_unlock(&dm->lock);
		return -EBUSY; /* don't FLR under a live frame */
	}
	/* drop bindings + chains before the wipe (caller-holds-lock variant) */
	__ab_dma_release(ab, true);
	mutex_unlock(&dm->lock);

	dev_info(&ab->pdev->dev, "hard reset: issuing FLR\n");
	ret = pci_reset_function(ab->pdev);
	if (ret) {
		dev_err(&ab->pdev->dev, "hard reset: FLR failed (%d)\n", ret);
		return ret;
	}

	ret = ab_bringup(ab);
	if (ret)
		dev_err(&ab->pdev->dev, "hard reset: bring-up after FLR failed (%d)\n", ret);
	else
		dev_info(&ab->pdev->dev, "hard reset: OK — re-bind regions and resubmit\n");
	return ret;
}
