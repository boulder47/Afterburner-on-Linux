// SPDX-License-Identifier: GPL-2.0
/*
 * ab_dma.c — frame submission engine (Phase 3u.2).
 *
 * Byte-exact port of the kext's submit path, decoded in FINDINGS §21/§25/§26 and
 * UC_METHODS §18:
 *   descriptor (32B, arm64e layout — fn@0x9c5c):
 *     +0x00 ctrl = 0xAD4B0000|((N-2-i)<<8) for i<N-1; final desc = 0xAD4B0013
 *     +0x04 segment length (bytes)
 *     +0x08 data PA — input chain (user_data=NULL path, @0xa124 w25=dir=0)
 *     +0x10 data PA — batch receive chain (dir=1)
 *     +0x18 64-bit next-desc PA; final = 0 (eoc)
 *   doorbell fn@0x6800: clear-before-ring, 3-word staging bank (lo/hi/local_val):
 *     send : BAR1+0x4080..C, ring BAR1+4      = 0xFFE7F
 *     batch: BAR1+0x5080..C, ring BAR1+0x1004 = 0x80FFE7F
 *   completion fn@0x5aec: status +0x2044 (bit0 valid, [31:1] count); ack#1 -> +0x2018
 *     BEFORE data read; count==0: data +0x44 (+0x48 R-to-C); count>=1: idx =
 *     BAR0[hdr+0x10]>>16, data +0x1044 (+0x1048 R-to-C); ack#2 -> +0x2014 AFTER.
 *     No bit0 gate — any non-zero status is a completion event (mode-12 rung 2:
 *     status=0x2 with bit0 clear was still handled).
 *   two-stage frame submit fn@0x985c: stage A SEND (pre-submit BAR0[hdr+0x18]=0x1F
 *     @0xa714, cmd_word -> BAR0[hdr+0xC] @0x9a14), stage B BATCH with receive chain;
 *     batch consumes a sequence number (@0xa988-0xa9a0).
 *
 * Hard constraints (live-verified):
 *   - <= AB_MAX_DESCS descriptors per chain (§28: engine consumed exactly 126 of a
 *     254-desc chain, then emitted malformed TLPs)
 *   - drain unacked completions before ringing — the engine ignores a new doorbell
 *     while +0x2044 holds a stale status (mode-10 run 1)
 *   - c2h DMA can lag the completion signal by ~1 s on large frames (§26.1)
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>

#include "include/ab.h"
#include "include/ab_regs.h"

#define AB_DESC_SIZE	32
#define AB_MAX_DESCS	126	/* §28 hard cap per chain */
#define AB_MIN_SEG	4096u	/* proven small-frame segment size (modes 9/13) */
#define AB_LOCAL_VAL	0x00000000u /* staging bank +8 word; kext = cmd[+0x18], semantics open — all successful runs used 0 */

#define AB_CTRL_BASE	0xAD4B0000u
#define AB_CTRL_FINAL	0xAD4B0013u
#define AB_DESC_PA_IN	0x08	/* input chain data PA offset */
#define AB_DESC_PA_OUT	0x10	/* batch receive chain data PA offset */

/* kext fn@0x9c5c chain builder (@0xa0f0-0xa164): split source segments into
 * fixed-size descriptors, each carrying its own data PA and next-desc link. */
static u32 ab_build_chain(struct ab_device *ab, struct ab_chain *ch,
			  const dma_addr_t *pas, const size_t *lens, int nseg_src,
			  u32 seg_size, u32 pa_off)
{
	u64 total = 0;
	int c;

	for (c = 0; c < nseg_src; c++)
		total += lens[c];
	if (!total || !nseg_src)
		return 0;

	/* descriptors = sum over source segments — a chunk boundary can fall
	 * mid-descriptor, so total/seg undercounts for multi-segment sources */
	ch->n = 0;
	for (c = 0; c < nseg_src; c++)
		ch->n += (u32)((lens[c] + seg_size - 1) / seg_size);
	if (!ch->n || ch->n > AB_MAX_DESCS) {
		dev_err(&ab->pdev->dev, "chain: %d descs for %llu B exceeds cap\n",
			ch->n, (unsigned long long)total);
		return 0;
	}

	ch->virt = dma_alloc_coherent(&ab->pdev->dev, ch->n * AB_DESC_SIZE,
				      &ch->pa, GFP_KERNEL);
	if (!ch->virt)
		return 0;
	memset(ch->virt, 0, ch->n * AB_DESC_SIZE);

	{
		u32 i = 0;

		for (c = 0; c < nseg_src && i < ch->n; c++) {
			size_t coff = 0;

			while (coff < lens[c]) {
				u32 *d = (u32 *)(ch->virt + i * AB_DESC_SIZE);
				u32 len = min(seg_size, (u32)(lens[c] - coff));

				d[0] = (i == ch->n - 1) ? AB_CTRL_FINAL :
					(AB_CTRL_BASE | ((ch->n - 2 - i) << 8));
				d[1] = len;
				*(u64 *)(d + pa_off / 4) = pas[c] + coff;
				*(u64 *)(d + 0x18 / 4) = (i < ch->n - 1) ?
					ch->pa + (i + 1) * AB_DESC_SIZE : 0; /* eoc */
				coff += len;
				i++;
			}
		}
	}
	return ch->n;
}

static void ab_chain_free(struct ab_device *ab, struct ab_chain *ch)
{
	if (ch->virt) {
		dma_free_coherent(&ab->pdev->dev, ch->n * AB_DESC_SIZE,
				  ch->virt, ch->pa);
		memset(ch, 0, sizeof(*ch));
	}
}

/* kext fn@0x6800 — clear-before-ring doorbell with 3-word staging bank. */
static void ab_ring(struct ab_device *ab, dma_addr_t head_pa, u32 bank_off,
		    u32 db_off, u32 ring_val)
{
	wmb();	/* descriptor stores (coherent mem) visible before the MMIO ring */
	ab_write(ab, AB_BAR1, db_off, 0);			/* clear doorbell first */
	ab_write(ab, AB_BAR1, bank_off, lower_32_bits(head_pa));
	ab_write(ab, AB_BAR1, bank_off + 4, upper_32_bits(head_pa));
	ab_write(ab, AB_BAR1, bank_off + 8, AB_LOCAL_VAL);	/* req[+0x18] */
	wmb();
	ab_write(ab, AB_BAR1, db_off, ring_val);			/* RING */
}

/* kext fn@0x5aec — one completion event for a non-zero status word.
 * Returns the data word (+0x44 or +0x1044). */
static u32 ab_handle_completion(struct ab_device *ab, u32 s)
{
	u32 count = s >> 1;
	u32 data = 0;

	ab_write(ab, AB_BAR1, AB_B1_ACK_0, s);	/* ack#1 (@0x63ac), before data */
	if (!count) {			/* send-channel completion (status=0x1) */
		data = ab_read(ab, AB_BAR1, AB_B1_DATA_SEND);
		ab_read(ab, AB_BAR1, AB_B1_DATA_SEND + 4); /* wordB R-to-C */
	} else if (count == 1) {	/* batch-channel completion (status=0x2) */
		u32 h10 = ab_read(ab, AB_BAR0, ab->hdr_off + 0x10);

		data = ab_read(ab, AB_BAR1, AB_B1_DATA_BATCH);
		ab_read(ab, AB_BAR1, AB_B1_DATA_BATCH + 4); /* wordB R-to-C */
		dev_info(&ab->pdev->dev, "completion: count=1 BAR0[hdr+0x10]=0x%08x idx=%u data(+0x1044)=0x%08x\n",
			 h10, h10 >> 16, data);
	}
	/* count>1: no per-item read (kext @0x6568) */
	ab_write(ab, AB_BAR1, AB_B1_ACK_1, s);	/* ack#2 (@0x666c), after data */

	if (!count) {
		/* kext fn@0x985c (§25.4): send-channel completion writes the
		 * firmware-visible "frame done" signal to BAR0[hdr+0xC]:
		 * [31:16] = (C8 - stampBase), where C8 already includes this
		 * frame's batch-ring increment and stampBase was set by the last
		 * reset() -> delta is 1 for sequential frames. Low half from VT
		 * slot fields (absent here -> 0). */
		u32 fw = ((ab->dma.seq - ab->dma.done_seq) & 0xFFFFu) << 16;

		ab_write(ab, AB_BAR0, ab->hdr_off + 0xC, fw);
		dev_dbg(&ab->pdev->dev, "frame done: BAR0[hdr+0xC]=0x%08x (rb 0x%08x)\n",
			fw, ab_read(ab, AB_BAR0, ab->hdr_off + 0xC));
	}
	return data;
}

/* Pre-submit drain — the engine ignores a new doorbell while +0x2044 holds an
 * unacked completion (mode-10 run 1). Handle any pending events first. */
static void ab_drain(struct ab_device *ab)
{
	int d;

	for (d = 0; d < 50; d++) {
		u32 s = ab_read(ab, AB_BAR1, AB_B1_COMPLETION);

		if (!s)
			break;
		dev_info(&ab->pdev->dev, "pre-submit drain: pending status=0x%08x — handling\n", s);
		ab_handle_completion(ab, s);
		mdelay(5);
	}
}

/* Segment size selection (V3, plan §8.5): adaptive default starts at the
 * proven 4KB and doubles until the chain fits the §28 cap; out_seg=N overrides
 * with a fixed N-byte segment on BOTH chains (clamped upward to the cap if
 * needed). kext parity knob: ~4-16 MB segments keep multi-MB frames at
 * single-digit descriptor counts. */
static u32 ab_out_seg = 0;
module_param_named(out_seg, ab_out_seg, uint, 0644);
MODULE_PARM_DESC(out_seg, "fixed descriptor segment size in bytes for both chains (0=adaptive from 4KB)");

/* V5 validation hook (plan.md §8.5): ignore completions for N ms after the
 * doorbell to simulate a firmware hang and exercise the sleep_timer port's
 * timeout path (dev_err + ab_frame_reset + -ETIMEDOUT). 0 = disabled. */
static int ab_test_hang_ms;
module_param(ab_test_hang_ms, int, 0644);
MODULE_PARM_DESC(ab_test_hang_ms, "V5 test hook: ignore completions for N ms after doorbell (0=off)");

static u32 ab_pick_seg(u64 total)
{
	u32 seg = ab_out_seg ? ab_out_seg : AB_MIN_SEG;

	while ((total + seg - 1) / seg > AB_MAX_DESCS && (seg << 1) >= seg)
		seg <<= 1;
	return seg;
}

/* Two-stage frame submit — kext fn@0x985c. in_pa/out_pa are coherent DMA PAs of
 * the user's mapped input/output regions. Blocks for up to poll_ms (default 2 s)
 * plus a 1 s c2h settle (§26.1). Returns 0 on completion, -ETIMEDOUT if no
 * completion arrived in the window. */
/* ------------------------------------------------------------------ *
 * Per-frame teardown — kext AB::reset @0xaea8 (UC_METHODS §10.3).
 *
 * The kext calls this after EVERY completed frame (query stub) and on
 * timeout/hang (sleep_timer). Without it the firmware's internal decode /
 * c2h source position keeps advancing between submits: live test 2026-11-09
 * showed garbage from frame #2 onward with a 512B hole walking +8KB per
 * frame, even with byte-identical persistent descriptor chains (E1 refuted).
 *
 * Sequence (§10.3): clear both doorbells -> decoder disable -> wait for
 * c2h/h2c status drain -> decoder re-enable until ready bit -> reset
 * completion tracking. Also the AER MalfTLP mitigation from §28 (decoder
 * disabled between frames, not left running).
 * ------------------------------------------------------------------ */
static void ab_frame_reset(struct ab_device *ab)
{
	struct ab_dma_state *dm = &ab->dma;
	u32 s;

	/* 1. clear both doorbells */
	ab_write(ab, AB_BAR1, AB_B1_DOORBELL_SEND, 0);
	ab_write(ab, AB_BAR1, AB_B1_DOORBELL_BATCH, 0);

	if (!ab->ready)
		return;

	/* 2. decoder disable (BAR0[hdr+4]) */
	ab_write(ab, AB_BAR0, ab->hdr_off + AB_DEC_ENABLE, 0);

	/* 3. wait for both channel status words to drain (bit0 = valid).
	 * kext: IOSleep(1000ms) once if still set; we poll up to ~2 s. */
	s = ab_read(ab, AB_BAR1, AB_B1_DATA_BATCH);	/* c2hStatus +0x1044 */
	if (s & 1) {
		int i;

		for (i = 0; i < 20 && (ab_read(ab, AB_BAR1, AB_B1_DATA_BATCH) & 1); i++)
			msleep(100);
		s = ab_read(ab, AB_BAR1, AB_B1_DATA_BATCH);
		if (s)
			dev_warn(&ab->pdev->dev, "reset: c2hStatus still 0x%08x\n", s);
	}
	s = ab_read(ab, AB_BAR1, AB_B1_DATA_SEND);	/* h2cStatus +0x44 */
	if (s & 1) {
		int i;

		for (i = 0; i < 20 && (ab_read(ab, AB_BAR1, AB_B1_DATA_SEND) & 1); i++)
			msleep(100);
		s = ab_read(ab, AB_BAR1, AB_B1_DATA_SEND);
		if (s)
			dev_warn(&ab->pdev->dev, "reset: h2cStatus still 0x%08x\n", s);
	}

	/* 4. decoder re-enable until ready bit ([hdr+8] & 1). kext loops with
	 * IOSleep(100ms) and re-writes enable each pass; cap at ~5 s here. */
	{
		int i, tries = 0;

		for (i = 0; i < 50; i++) {
			ab_write(ab, AB_BAR0, ab->hdr_off + AB_DEC_ENABLE, 1);
			if (ab_read(ab, AB_BAR0, ab->hdr_off + AB_DEC_READY) & 1)
				break;
			msleep(100);
			tries++;
		}
		dev_info(&ab->pdev->dev, "reset: decoder status=0x%08x after %d ms\n",
			 ab_read(ab, AB_BAR0, ab->hdr_off + AB_DEC_READY), tries * 100);
	}

	/* 5. reset completion tracking (kext: stampBase = frameNum) */
	dm->done_seq = dm->seq;
	dm->in_flight = 0;
}

int ab_submit_frame(struct ab_device *ab,
		    const dma_addr_t *in_pas, const size_t *in_lens, int n_in,
		    const dma_addr_t *out_pas, const size_t *out_lens, int n_out,
		    u32 in_len, u32 out_len, u32 poll_ms,
		    u32 *out_status, u32 *out_data)
{
	struct ab_dma_state *dm = &ab->dma;
	u32 last = 0, final_status = 0, data_word = 0, ncomp = 0;
	bool fresh_in = false, fresh_out = false;
	unsigned long deadline;

	if (!ab->ready)
		return -ENODEV;
	if (!in_len || !out_len || n_in < 1 || n_out < 1)
		return -EINVAL;

	mutex_lock(&dm->lock);
	if (dm->in_flight >= AB_MAX_IN_FLIGHT) { /* kext 0xE000_02D8 "no space" */
		mutex_unlock(&dm->lock);
		return -ENOSPC;
	}

	/* Persistent chains (kext parity): build once per segment list, reuse
	 * for every submit. Per-submit alloc/free broke decoding from frame #2
	 * on (live test 2026-11-09 — FINDINGS §31). Keyed on first PA + total
	 * len + segment count: a rebind frees the chains, and an in_off change
	 * moves the first PA. */
	if (!dm->in_ch.virt || dm->ch_in_pa != in_pas[0] ||
	    dm->ch_in_len != in_len || dm->ch_in_nseg != n_in) {
		ab_chain_free(ab, &dm->in_ch);
		if (!ab_build_chain(ab, &dm->in_ch, in_pas, in_lens, n_in,
				    ab_pick_seg(in_len), AB_DESC_PA_IN))
			goto err_nomem;
		dm->ch_in_pa = in_pas[0];
		dm->ch_in_len = in_len;
		dm->ch_in_nseg = n_in;
		fresh_in = true;
	}
	if (!dm->out_ch.virt || dm->ch_out_pa != out_pas[0] ||
	    dm->ch_out_len != out_len || dm->ch_out_nseg != n_out) {
		ab_chain_free(ab, &dm->out_ch);
		if (!ab_build_chain(ab, &dm->out_ch, out_pas, out_lens, n_out,
				    ab_pick_seg(out_len), AB_DESC_PA_OUT))
			goto err_nomem;
		dm->ch_out_pa = out_pas[0];
		dm->ch_out_len = out_len;
		dm->ch_out_nseg = n_out;
		fresh_out = true;
	}

	dev_info(&ab->pdev->dev, "submit: in_head=%pad out_head=%pad in_len=%u out_len=%u in_descs=%u out_descs=%u (in_segs=%d out_segs=%d%s)\n",
		 &in_pas[0], &out_pas[0], in_len, out_len,
		 dm->in_ch.n, dm->out_ch.n, n_in, n_out,
		 (fresh_in || fresh_out) ? " [chains built]" : "");

	if (fresh_in || fresh_out) { /* ground-truth dump of the descriptors */
		u32 i;

		for (i = 0; i < dm->in_ch.n && i < 6; i++) {
			const u32 *d = (const u32 *)(dm->in_ch.virt + i * AB_DESC_SIZE);

			dev_info(&ab->pdev->dev, "  in[%u]: %08x %08x | pa=%016llx | next=%016llx\n",
				 i, d[0], d[1],
				 (unsigned long long)le64_to_cpu(*(const u64 *)(d + 2)),
				 (unsigned long long)le64_to_cpu(*(const u64 *)(d + 6)));
		}
		for (i = 0; i < dm->out_ch.n && i < 6; i++) {
			const u32 *d = (const u32 *)(dm->out_ch.virt + i * AB_DESC_SIZE);

			dev_info(&ab->pdev->dev, "  out[%u]: %08x %08x | pa=%016llx | next=%016llx\n",
				 i, d[0], d[1],
				 (unsigned long long)le64_to_cpu(*(const u64 *)(d + 4)),
				 (unsigned long long)le64_to_cpu(*(const u64 *)(d + 6)));
		}
	}

	ab_drain(ab);

	/* ---- Stage A (SEND) — kext request builder @0xa8c8 ---- */
	if (ab->ready)
		ab_write(ab, AB_BAR0, ab->hdr_off + 0x18, 0x1F); /* pre-submit (@0xa714) */

	/* cmd_word = ((C8 - stampBase)<<16)|(p3<<8)|(p2<<4)|p1 — kext §15.2/§25.4.
	 * C8 increments once per batch ring (my seq++ below); stampBase is re-based
	 * to C8 by ab_frame_reset() after every frame -> delta is 0 for sequential
	 * submits. p1..p3 are VT slot fields set at bindFrame; no VT here, so 0. */
	ab_write(ab, AB_BAR0, ab->hdr_off + 0xC,
		 ((dm->seq - dm->done_seq) << 16) & 0xffff0000u); /* @0x9a14 */

	dm->in_flight++;
	ab_ring(ab, dm->in_ch.pa, AB_B1_STAGE_SEND, AB_B1_DOORBELL_SEND, 0xFFE7F);

	/* ---- Stage B (BATCH) — receive bank with fresh out chain ---- */
	dm->seq++;	/* batch consumes a seq (@0xa988-0xa9a0) */
	ab_ring(ab, dm->out_ch.pa, AB_B1_STAGE_BATCH, AB_B1_DOORBELL_BATCH, 0x80FFE7F);

	/* ---- Completion poll (kext fn@0x5aec semantics: any non-zero status).
	 * After a handled event the dual-ack clears +0x2044 to 0 (mode 9), so
	 * `last` resets on the zero read and a repeat value is re-handled. */
	if (!poll_ms)
		poll_ms = 2000;
	deadline = jiffies + msecs_to_jiffies(poll_ms);
	{
		unsigned long hang_until = ab_test_hang_ms ?
			jiffies + msecs_to_jiffies(ab_test_hang_ms) : 0;

		/* Flush a stale completion left by a previously timed-out frame:
		 * an un-acked valid bit would otherwise be counted as this frame's
		 * event (the dual-ack in ab_handle_completion clears +0x2044). */
		u32 stale = ab_read(ab, AB_BAR1, AB_B1_COMPLETION);

		if (stale) {
			dev_info(&ab->pdev->dev, "submit: flushing stale completion 0x%08x\n", stale);
			ab_handle_completion(ab, stale);
		}

		while (time_before(jiffies, deadline)) {
			u32 s;

			if (time_before(jiffies, hang_until)) {
				usleep_range(2000, 4000);
				continue;	/* V5 hook: simulate firmware hang */
			}
			s = ab_read(ab, AB_BAR1, AB_B1_COMPLETION);

			if (s && s != last) {
				last = s;
				ncomp++;
				final_status = s;
				data_word = ab_handle_completion(ab, s);
			} else if (!s) {
				last = 0;	/* cleared — ready for next event */
			}
			usleep_range(2000, 4000);
		}
	}

	if (ncomp == 0) {
		dev_err(&ab->pdev->dev, "submit: no completion in %u ms window\n", poll_ms);
		dm->in_flight--;
		/* kext sleep_timer path: reset on hang too (§10.2) */
		ab_frame_reset(ab);
		mutex_unlock(&dm->lock);
		return -ETIMEDOUT;
	}

	/* c2h DMA can lag the completion signal by ~1 s on large frames (§26.1) */
	msleep(1000);

	if (out_status)
		*out_status = final_status;
	if (out_data)
		*out_data = data_word;
	dm->in_flight--;
	dm->last_done = true;
	dev_info(&ab->pdev->dev, "submit: %u completion(s), last status=0x%08x data=0x%08x\n",
		 ncomp, final_status, data_word);

	/* Per-frame teardown (kext query stub -> AB::reset) — REQUIRED for
	 * frame #2+ correctness; see ab_frame_reset(). */
	ab_frame_reset(ab);
	mutex_unlock(&dm->lock);
	return 0;

err_nomem:
	mutex_unlock(&dm->lock);
	return -ENOMEM;
}

/* Free persistent chains (and optionally the bound data regions) — called on
 * re-bind, unbindAll, and module remove. Caller holds no lock; submit is
 * serialized by dm->lock so this must not race an in-flight frame. */
void ab_dma_release(struct ab_device *ab, bool free_bufs)
{
	struct ab_dma_state *dm = &ab->dma;

	mutex_lock(&dm->lock);
	ab_chain_free(ab, &dm->in_ch);
	ab_chain_free(ab, &dm->out_ch);
	dm->ch_in_pa = dm->ch_out_pa = 0;
	dm->ch_in_len = dm->ch_out_len = 0;
	dm->ch_in_nseg = dm->ch_out_nseg = 0;
	if (free_bufs) {
		int i;

		for (i = 0; i < dm->in.n_chunks; i++)
			if (dm->in.chunks[i].virt)
				dma_free_coherent(&ab->pdev->dev,
						  dm->in.chunks[i].len,
						  dm->in.chunks[i].virt,
						  dm->in.chunks[i].pa);
		for (i = 0; i < dm->out.n_chunks; i++)
			if (dm->out.chunks[i].virt)
				dma_free_coherent(&ab->pdev->dev,
						  dm->out.chunks[i].len,
						  dm->out.chunks[i].virt,
						  dm->out.chunks[i].pa);
		memset(&dm->in, 0, sizeof(dm->in));
		memset(&dm->out, 0, sizeof(dm->out));
	}
	dm->seq = 0;
	dm->done_seq = 0;
	dm->in_flight = 0;
	dm->last_done = false;
	mutex_unlock(&dm->lock);
}

/* Non-blocking peek at +0x2044 (queryCompletion): if a completion is pending,
 * handle it kext-exact and report. Returns 0 with status/data filled, or
 * -EAGAIN when nothing is pending. */
int ab_query_completion(struct ab_device *ab, u32 *status, u32 *data_word)
{
	u32 s = ab_read(ab, AB_BAR1, AB_B1_COMPLETION);

	if (!s) {
		if (status)
			*status = 0;
		return -EAGAIN;
	}
	if (status)
		*status = s;
	if (data_word)
		*data_word = ab_handle_completion(ab, s);
	return 0;
}
