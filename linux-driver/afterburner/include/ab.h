/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _AB_H_
#define _AB_H_

#include <linux/types.h>
#include <linux/pci.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/dma-mapping.h>

/* kext self->+0xc0 — max outstanding frames (wait path @0x996c) */
#define AB_MAX_IN_FLIGHT	32

struct ab_buf {
	void	*virt;	/* coherent DMA buffer (CPU side) */
	dma_addr_t	pa;
	size_t		size;
};

/* 32B descriptor chain (arm64e layout, fn@0x9c5c). PERSISTENT across submits
 * (kext parity): the engine mis-decodes from submit #2 onward when chains are
 * allocated/freed per submit — live test 2026-11-09, see FINDINGS §31. */
struct ab_chain {
	void	*virt;	/* coherent desc buffer (N*32B) */
	dma_addr_t	pa;
	u32	n;
};

struct ab_dma_state {
	struct mutex	lock;	/* one frame at a time (kext IOLock @+0x12b8) */
	u32	seq;		/* self->+0xc8 — sequence base counter */
	u32	done_seq;	/* frames completed — value for the BAR0[hdr+0xC]
			 * "frame done" write (kext fn@0x985c, §25.4) */
	u32	in_flight;	/* outstanding frames, max AB_MAX_IN_FLIGHT */
	bool	last_done;	/* last submit completed (getPercent v1 stub) */
	struct ab_buf in;	/* input region: compressed frame (mmap offset 0) */
	struct ab_buf out;	/* output region: decoded YUV422 c2h target (offset 1) */
	struct ab_chain in_ch;	/* persistent input chain, keyed on ch_in_pa/len */
	struct ab_chain out_ch;	/* persistent receive chain, keyed on ch_out_pa/len */
	dma_addr_t	ch_in_pa, ch_out_pa;
	u32		ch_in_len, ch_out_len;
};

struct ab_device {
	struct pci_dev	*pdev;
	void __iomem	*bar0;	/* 128K — control/status regs (FINDINGS §7–§9) */
	void __iomem	*bar1;	/* 64K  — command/doorbell/status surface (§21) */

	u32	hdr_off;	/* decoder section header offset in BAR0 (dir walk) */
	bool	fw_ok;	/* fw version gate passed */
	bool	ready;	/* decoder ready bit set after bring-up */

	struct ab_dma_state dma;

	/* chardev (ab_chardev.c) — /dev/afterburnerN */
	struct cdev	cdev;
	struct device	*devnode;
};

/* ab_regs.c — thin accessors over the two ioremap'd BARs */
u32 ab_read(struct ab_device *ab, int bar, u32 off);
void ab_write(struct ab_device *ab, int bar, u32 off, u32 val);

/* ab_engine.c — decoder bring-up / stop / recovery (kext-exact) */
int  ab_bringup(struct ab_device *ab);
void ab_stop(struct ab_device *ab);
bool ab_recover(struct ab_device *ab);

/* ab_dma.c — frame submission engine (Phase 3u.2, kext fn@0x985c port) */
int ab_submit_frame(struct ab_device *ab, dma_addr_t in_pa, u32 in_len,
		    dma_addr_t out_pa, u32 out_len, u32 poll_ms,
		    u32 *out_status, u32 *out_data);
int ab_query_completion(struct ab_device *ab, u32 *status, u32 *data_word);
void ab_dma_release(struct ab_device *ab, bool free_bufs);

/* ab_chardev.c — /dev/afterburnerN + ioctls mirroring the UC sMethods ABI */
int  ab_chardev_register(struct ab_device *ab);
void ab_chardev_unregister(struct ab_device *ab);

#endif /* _AB_H_ */
