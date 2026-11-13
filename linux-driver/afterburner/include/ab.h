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

/* Space pool capacity for the bindFrame reserve model (UC_METHODS §6). The
 * kext never writes its own cap field (AB+0x12F0: full __text scan shows only
 * two loads, zero stores — FINDINGS §39), so we set an explicit budget well
 * above any single-card workload (4K ProRes 422 bind ≈ in 8 MiB + out 36 MiB). */
#define AB_SPACE_CAP	(512ULL << 20)

/* No-IOMMU hosts: large coherent allocations need physically contiguous
 * memory and fail; regions fall back to chunked allocations (FINDINGS §33). */
#define AB_MAX_CHUNKS 64
#define AB_MAX_SEGS	48	/* per-submit segment lists (ioctl stack budget) */

struct ab_chunk {
	dma_addr_t	pa;
	void		*virt;
	size_t		len;
};

struct ab_buf {
	void		*virt;	/* first chunk vaddr (NULL when unbound) */
	dma_addr_t	pa;		/* first chunk PA */
	size_t		size;		/* logical total bytes */
	int		n_chunks;	/* 1 = single allocation */
	struct ab_chunk chunks[AB_MAX_CHUNKS];
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

	/* getPercent time accounting — kext-exact port of the trio decoded in
	 * UC_METHODS §11 + tmp/dis_ab_updateIdleTime.txt / dis_ab_cardStatus_getPercent.txt:
	 *   acc_e0 = AB+0xE0  (u64 ns) — intervals ending at a SUBMIT, i.e. [completion ->
	 *            next submit]: the card waiting for work. Banked by updateIdleTime @0x9be0.
	 *   acc_e8 = AB+0xE8  (u64 ns) — intervals ending at a COMPLETION, i.e. [submit ->
	 *            completion]: the FPGA decoding. Banked by updateActiveTime @0xbe64.
	 *   last_sample = AB+0xF8 (last transition/sample time; kext timeval us, we use ns)
	 *   idle_flag   = AB+0x12E0 — 1 between submit and completion (kext's "idle" flag;
	 *            the naming is inverted vs physical state — see FINDINGS §39).
	 * getPercent: remaining = now - last_sample goes to acc_e8 if idle_flag else acc_e0;
	 *   *out = total ? acc_e8*100/total : 0 (decode duty cycle over the window since the
	 *            previous call); both accumulators then reset to 0, last_sample := now.
	 * Protected by time_lock (never takes dm->lock). */
	u64	acc_e0;
	u64	acc_e8;
	ktime_t	last_sample;
	bool	idle_flag;
	raw_spinlock_t time_lock;

	/* checkForSpace space pool — kext model (UC_METHODS §15.2/§18): bindFrame
	 * reserves the bound DMA bytes, unbindAll releases all. used/cap are the
	 * in+out region sizes after a successful bind. Protected by dm->lock. */
	u64	space_used;
	u64	space_cap;

	struct ab_buf in;	/* input region: compressed frame (mmap offset 0) */
	struct ab_buf out;	/* output region: decoded YUV422 c2h target (offset 1) */
	struct ab_chain in_ch;	/* persistent input chain, keyed on ch_in_pa/len/nseg */
	struct ab_chain out_ch;	/* persistent receive chain, keyed on ch_out_pa/len/nseg */
	dma_addr_t	ch_in_pa, ch_out_pa;
	u32		ch_in_len, ch_out_len;
	u32		ch_in_nseg, ch_out_nseg;
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
int  ab_hard_reset(struct ab_device *ab);	/* FLR + full re-bringup (B2) */

/* ab_dma.c — frame submission engine (Phase 3u.2, kext fn@0x985c port).
 * in/out are (pa,len) segment lists — one entry per coherent chunk the region
 * spans (nseg==1 for single allocations); in_len/out_len are logical totals. */
int ab_submit_frame(struct ab_device *ab,
		    const dma_addr_t *in_pas, const size_t *in_lens, int n_in,
		    const dma_addr_t *out_pas, const size_t *out_lens, int n_out,
		    u32 in_len, u32 out_len, u32 poll_ms,
		    u32 *out_status, u32 *out_data);
/* B1 async pair (kext IRQ-driven model): SUBMIT_ASYNC rings both doorbells and
 * returns immediately; WAIT_COMPLETION blocks until the next completion word,
 * then does c2h settle + per-frame reset. Depth 1 unreaped frame (-EBUSY). */
int ab_submit_async(struct ab_device *ab,
		    const dma_addr_t *in_pas, const size_t *in_lens, int n_in,
		    const dma_addr_t *out_pas, const size_t *out_lens, int n_out,
		    u32 in_len, u32 out_len);
int ab_wait_completion(struct ab_device *ab, u32 timeout_ms,
		       u32 *out_status, u32 *out_data);
/* getPercent v2 (kext-exact formula, UC_METHODS §11) */
u32 ab_get_percent(struct ab_device *ab);
int ab_query_completion(struct ab_device *ab, u32 *status, u32 *data_word);
/* Drop bindings/chains/counters. __-variant: caller holds dm->lock (used by
 * BIND_FRAME's reserve+alloc critical section and ab_hard_reset). */
void __ab_dma_release(struct ab_device *ab, bool free_bufs);
void ab_dma_release(struct ab_device *ab, bool free_bufs);

/* ab_chardev.c — /dev/afterburnerN + ioctls mirroring the UC sMethods ABI */
int  ab_chardev_register(struct ab_device *ab);
void ab_chardev_unregister(struct ab_device *ab);

/* ab_sysfs.c — read-only attributes under /sys/class/afterburner/afterburnerN */
int  ab_sysfs_register(struct ab_device *ab);
void ab_sysfs_unregister(struct ab_device *ab);

#endif /* _AB_H_ */
