// SPDX-License-Identifier: GPL-2.0
/*
 * afterburner_submit2.c — Phase 3i–3k: BAR1 DMA submit + decoder bring-up.
 *
 * Supersedes afterburner_submit.c (Phase 3b, BAR0 geometry — WRONG) and the
 * staging offsets in afterburner_init_seq.c (+0x3080 — never verified; the kext
 * __text contains NO write to +0x3080 anywhere).
 *
 * Protocol decoded from AppleAfterburner arm64e (two independent code sites agree):
 *   submitDataGated @0x97c4-0x97f0 and completion-region send path @0x68bc-0x68e8:
 *
 *     SEND  : BAR1[+4]    = 0            (clear doorbell)
 *             BAR1[+0x4080] = desc_pa_lo
 *             BAR1[+0x4084] = desc_pa_hi
 *             BAR1[+0x4088] = local_val  (kext: rec+0x18; meaning open, try 0)
 *             BAR1[+4]    = 0xFFE7F      (ring doorbell)
 *     RECV  : BAR1[+0x1004]= 0 / +0x5080/4/8 staging / ring 0x80FFE7F
 *
 *   Completion handler @0x639c-0x666c (re-traced this session):
 *     status = BAR1[+0x2044]             (bit0=valid, bits[31:1]=count)
 *     ack    : BAR1[+0x2018] = status    (@0x63ac) AND BAR1[+0x2014] = status (@0x666c)
 *     count==0 (single-shot): data = BAR1[+0x44]; read-to-clear BAR1[+0x48]
 *       Only bit0 of `data` is tested by the handler: tbnz data,#0 -> return
 *       WITHOUT per-frame processing; bit0 clear = normal completion path.
 *     count==1 (batch): data = BAR1[+0x1044]; read-to-clear +0x1048
 *
 *   Descriptor chain in system RAM, 32 bytes each — arm64e __text chain-builder
 *   @0xa0f0-0xa164 (inside fn @0x9c5c; segment list at self->+0x11d8, N entries
 *   of {dataPA@+0, len@+8}, N = self->+0x11e0>>4):
 *     [+0x00] = control word: tag 0xAD4B0000 | count<<8; last desc ORs in 0x13.
 *               For N segments, non-final desc i carries (N-2-i)<<8 — e.g.
 *               N=5 -> [0xAD4B0300, 0xAD4B0200, 0xAD4B0100, 0xAD4B0000,
 *               0xAD4B0013]. Single descriptor is exactly 0xAD4B0013.
 *     [+0x04] = segment length in bytes (raw value from seg[+8])
 *     [+0x08] = data buffer PA — SEND variant; the kext's user_data arg selects
 *               [+0x10] instead (cmp w25,#0 / csel eq). mode=3 proved the engine
 *               ignores the slot for single-shot.
 *     [+0x18] = 64-bit PA of the NEXT descriptor; 0 on the final one (eoc).
 *
 * DECODER BRING-UP (Phase 3k) — runtime enable function @0x7508-0x7b80:
 *   1. Firmware version gate: word at BAR0+0x00008 must have bits[31:16]==2 and
 *      bits[15:0] in {0x3E, 0x3F}. Live card reads 0x0002003E -> PASSES.
 *   2. Walk the section directory from BAR0+0x00000: entry word W at offset O;
 *      type = W & 0xFF (0 terminates); next offset = O + ((W>>14) & 0x3FFFC).
 *      First type==1 entry is the DECODER section header. Live card: found at
 *      BAR0+0x8000 (word 0x000b0101, stride 0x2C — entries +0x8000, +0x802C=0
 *      terminator; the walk is well-formed and we stop at the first type==1 hit,
 *      which is all the kext needs from it).
 *   3. Disable: BAR0[hdr+4] = 0; delay ~1ms (kext bl #0xe89c w0=1);
 *      Enable : BAR0[hdr+4] = 1.  ("Decoder Control Register" logs.)
 *   4. Init writer @0x5a3c-0x5ad0 (called by the enable fn AFTER enabling):
 *        BAR1[+0x90]=0xFFFFFE, BAR1[+0x1090]=0xFFFFFE, BAR1[+0x2010]=3
 *        BAR0[+0x1040]=0x2D002A80 [+0x1048 same] [+0x1050 same] [+0x1064 same]
 *        BAR0[+0x104C]=0x2A802800, BAR0[+0x1054]=0xBCD3BA4D (magic)
 *        BAR0[+0x1058]=0x2F802D00 [+0x1060 same] [+0x1068 same]
 *        BAR0[+0x106C]=0xBF58BCD3 (magic), BAR0[+0x1030]=0x7D, +0x1034=0x7D
 *        BAR0[+0x202C]=0, BAR0[+0x2030]=6, BAR0[+0x2034]=0   (PR block reset)
 *        BAR1[+0x2004]=0x16
 *      Phase 0 note: +0x1040..+0x106C already hold these exact values at cold
 *      boot (FPGA pre-populates them); only +0x1030/+0x1034 (0x3FFF->0x7D) and
 *      the PR block (+0x202C 7->0, +0x2030 7->6) actually change.
 *   5. Readiness: frame-submit path @~0xa700 checks bit0 of BAR0[hdr+8]
 *      (self->+0x158 = decoder header ptr; ldr w,[x8,#8]; tbz -> "Decoder not
 *      ready!! (0x%08x)"). Live card cold: 0x3E0FD000, bit0 clear. We poll it
 *      after bring-up for up to ~2s.
 *
 * NOTE: kext has a conditional "wake" pre-submit call (virtual method on
 * self->+0x90; log template VA 0x2326). Gated on state byte self->+0x12f8 —
 * NOT needed for a fresh submit (Phase 3i live run completed without it).
 *
 * FRAME PATH (mode=6) — decoded @0xa6fc-0xa714 (dump_sub.txt): when the ready
 * bit of BAR0[hdr+8] is SET, the kext writes BAR0[hdr+0x18] = 0x1F right
 * before submitting a frame. Our modes 1/3/5 skipped this write; mode=6 adds
 * it and feeds a REAL ProRes sample (raw QuickTime sample: [u32 BE size][
 * "icpf"][frame hdr w/ fmpg box, width@+8 height@+10][picture hdr][slices])
 * loaded from the file given by frame= — e.g. one 320x180 ProRes proxy frame
 * (20138 bytes) extracted from a .mov via stsz/stco.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/jiffies.h>

#define AB_VENDOR	0x106b
#define AB_DEVICE	0x205

static const char *out_path = "/tmp/submit2_result.txt";

/* 0 = snapshot only (safe), 1 = init + kext-exact submit,
 * 2 = submit-only (no init), 3 = init + dual-variant test (PA@+0x08 then PA@+0x10),
 * 4 = full decoder bring-up (version gate + walk + enable + init writer + ready poll),
 * 5 = bring-up + single-descriptor submit (compare data(+0x44) vs the 6 from modes 1/3),
 * 6 = bring-up + kext-exact pre-submit write (hdr+0x18=0x1F) + REAL ProRes frame
 *     payload loaded from frame=<path>,
 * 7 = single-shot LENGTH SWEEP: BAR1-only init (mode-1 style, no decoder
 *     bring-up — pattern submits worked without it in Phase 3i), then submit
 *     the same descriptor with len = 64..20138 to find where the engine stops
 *     consuming single-shot descriptors. Isolates length as the variable:
 *     mode=6's real frame (len=20138) was NOT consumed (+0x2010 stayed 3)
 *     while len=64 always is,
 * 8 = RECOVERY MATRIX: after mode=7 showed that even len=64 stopped being
 *     consumed (engine wedged since the un-consumed 20138-byte submit of
 *     mode=6 run #2), try a ladder of recoveries until a len=64 submit works
 *     again: full bring-up -> clear +0x2018 -> clear +0x2014 too -> PCIe FLR
 *     (pci_reset_function) + re-init. Logs which rung restored acceptance, if
 *     any. Rung A (as-is after bring-up) SUCCEEDED — the decoder disable/enable
 *     cycle is the wedge recovery; ack-register writes do not stick,
 * 9 = CHAIN-SUBMIT LADDER: full bring-up + kext-exact pre-submit write, then
 *     build a multi-segment descriptor chain (4KB segments, arm64e-verified
 *     layout) from the frame payload and try three submit variants in order,
 *     re-running the mode-8 recovery cycle after each failure:
 *       rung 1 = chain head -> batch bank (+0x5080), ring 0x80FFE7F
 *       rung 2 = chain head -> send bank (+0x4080), ring 0xFFE7F
 *       rung 3 = single descriptor (full length) -> batch bank, 0x80FFE7F
 *     Rung 2 produced status(+0x2044)=0x2 = count 1 with bit0 CLEAR — a real
 *     completion the old bit0-gated poll misread as timeout.
 * 10 = KEXT-EXACT CHAIN COMPLETION: full bring-up + pre-submit write, build the
 *     4KB-segment chain and submit it via the SEND bank (+0x4080, ring 0xFFE7F —
 *     the rung-2 config). Completion handling is byte-exact per fn @0x5aec:
 *     read status, ack#1 to +0x2018 BEFORE reading data, branch on count=st>>1
 *     (count==0 -> +0x44/+0x48 R-to-C; count==1 -> idx=[BAR0 hdr+0x10]>>16 and
 *     +0x1044/+0x1048 R-to-C; count>1 -> no read), ack#2 to +0x2014 AFTER.
 *     Polls at 5ms steps logging every status change (trajectory) so per-segment
 *     completion sequences are visible; each distinct non-zero status is handled
 *     once. Ends with the decoder error word BAR0[hdr+0x14] read + write-back
 *     (kext-exact R-to-C @0x63f8/0x63fc).
 * 11 = INSTRUMENTED DUAL-CHAIN RUN: full bring-up, drain any pending events
 *     kext-exact, then submit the frame as a SEND-bank chain (+0x4080, ring
 *     0xFFE7F, local_val=0) twice with long instrumented polls (10ms steps for
 *     poll_ms each; every status(+0x2044) or decoder-counter(BAR0[hdr+0x14])
 *     change logged, heartbeat ~5s):
 *       M11-A: 4KB segments -> N=5 descriptors
 *       M11-B: 2KB segments -> N=10 descriptors (same bank/doorbell/local_val)
 *     Purpose: measure chain-processing latency and determine whether the
 *     completion wordB(+0x48) is live per-event (= segment count walked) or
 *     sticky. Mode-9/10 runs showed chains DO complete but take longer than
 *     short poll windows, with completions arriving delayed across rmmod cycles.
 * 12 = BATCH-BANK CHAIN RUN (fast path): full bring-up + drain, then three
 *     instrumented submits reusing m11_submit:
 *       M12-A: batch bank (+0x5080, ring 0x80FFE7F @+0x1004), N=10 seg=2048
 *              -> expect status=0x1 count=0, wordB(+0x48)=10 (wordB=N live?)
 *       M12-B: batch bank, N=5 seg=4096 -> expect wordB=5 (reproducibility)
 *       M12-C: send bank (+0x4080, ring 0xFFE7F @+4), N=5 seg=4096
 *              -> measure clean send-bank latency (mode-11's A/B were confounded
 *                 by desc_buf reuse while A was in flight) */
static int mode = 1;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode, "0=snapshot, 1=init+submit, 2=submit-only, 3=dual-variant, 4=bring-up, 5=bring-up+submit, 6=bring-up+real-frame, 7=length-sweep, 8=recovery-matrix, 9=chain-ladder, 10=kext-exact-chain-completion, 11=instrumented-dual-chain, 12=batch-bank-chain");

/* mode 7: sweep sizes (last = the real frame size that timed out in mode 6) */
static const u32 sweep_sizes[] = { 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 20138 };
#define SWEEP_MAX 20138u

/* data payload size in bytes (single descriptor) */
static u32 data_size = 64;
module_param(data_size, uint, 0444);

/* mode 6: path to a raw ProRes sample file (full QuickTime sample incl. the
 * leading [u32 BE size]["icpf"] header). Overrides data_size with file size. */
static char *frame_path = "";
module_param(frame_path, charp, 0444);
MODULE_PARM_DESC(frame_path, "mode 6: raw ProRes sample file to DMA as payload");

#define MAX_FRAME_SIZE (4u * 1024 * 1024)

/* completion poll budget in ms (ping completes <1ms; a real frame decode may
 * take longer — pass poll_ms=2000 for mode 6). */
static int poll_ms = 50;
module_param(poll_ms, int, 0444);
MODULE_PARM_DESC(poll_ms, "completion poll budget in ms (default 50)");

static void log_line(const char *fmt, ...)
{
	static struct file *f;
	static loff_t pos;
	char buf[512];
	va_list ap;
	int len;

	if (!f) {
		f = filp_open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (IS_ERR(f)) return;
		pos = 0;
	}
	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	kernel_write(f, buf, len, &pos);
}

static void dump_regs(const char *tag, void __iomem *b1)
{
	static const struct { u32 off; const char *name; } regs[] = {
		{ 0x0000, "engine ctrl/status" },
		{ 0x0004, "send doorbell" },
		{ 0x0090, "chA mask" },
		{ 0x1004, "recv doorbell" },
		{ 0x1090, "chB mask" },
		{ 0x2004, "engine enable" },
		{ 0x2010, "eng ctrl" },
		{ 0x2014, "ack reg B (@0x666c)" },
		{ 0x2018, "completion ack" },
		{ 0x2044, "completion status" },
		{ 0x3080, "(old 3g guess)" },
		{ 0x4080, "send stage lo" },
		{ 0x4084, "send stage hi" },
		{ 0x4088, "send local_val" },
	};

	log_line("--- %s ---\n", tag);
	for (unsigned i = 0; i < ARRAY_SIZE(regs); i++)
		log_line("  BAR1+0x%04x %-20s = 0x%08x\n", regs[i].off,
		       regs[i].name, readl(b1 + regs[i].off));
}

/* BAR0 observables: PR/int block (init writes 0/6/0) + channel-block-0 counter. */
static void dump_bar0(const char *tag, void __iomem *b0)
{
	static const struct { u32 off; const char *name; } regs[] = {
		{ 0x0008, "fw version" },
		{ 0x1014, "status A (tail)" },
		{ 0x1020, "status B (tail)" },
		{ 0x1024, "status C (tail)" },
		{ 0x2014, "probe gate" },
		{ 0x202C, "int blk A" },
		{ 0x2030, "int blk B" },
		{ 0x2034, "counter slot 0" },
		{ 0x8014, "decoder counter" },
		{ 0x8018, "pre-submit reg (kext writes 0x1f)" },
	};

	log_line("--- %s (BAR0) ---\n", tag);
	for (unsigned i = 0; i < ARRAY_SIZE(regs); i++)
		log_line("  BAR0+0x%04x %-20s = 0x%08x\n", regs[i].off,
		       regs[i].name, readl(b0 + regs[i].off));
}

/* Dump the decoder channel block (hdr .. hdr+0x30) — header word, control reg,
 * status/ready (+8), counter (+0x14), per-channel config (+0x24/+0x28). */
static void dump_channel(const char *tag, void __iomem *b0, u32 hdr_off)
{
	log_line("--- %s (BAR0+0x%04x channel block) ---\n", tag, hdr_off);
	for (u32 o = 0; o <= 0x30; o += 4)
		log_line("  BAR0+0x%04x = 0x%08x\n", hdr_off + o, readl(b0 + hdr_off + o));
}

/* Fill the single-descriptor chain kext-exact (chain-builder @0x9f9c-0xa164):
 *   [+0]=ctrl 0xAD4B0013 (single desc: count=0, last-flag), [+4]=len bytes,
 *   [pa_off]=data PA (send: +8; recv variant under test: +0x10), [+0x18]=eoc 0. */
static void fill_desc(u8 *desc_buf, dma_addr_t data_pa, u32 pa_off)
{
	u32 *d = (u32 *)desc_buf;

	memset(desc_buf, 0, 4096);
	d[0x00 / 4] = 0xAD4B0013;	/* tag | count=0<<8 | last-flag 0x13 */
	d[0x04 / 4] = data_size;	/* segment length in bytes (seg[+8]) */
	*(u64 *)(desc_buf + pa_off) = data_pa;
	d[0x18 / 4] = 0;			/* end-of-chain marker */
}

/* One full submit: stage, ring, poll ~50ms, read completion pair, dual-ack.
 * Returns the +0x2044 status word (bit0=valid). */
static u32 do_submit(const char *tag, void __iomem *bar1, void __iomem *bar0,
		     u8 *desc_buf, dma_addr_t desc_pa, dma_addr_t data_pa,
		     u32 pa_off)
{
	u32 status = 0;

	log_line("\n--- SUBMIT %s (data PA at desc+%x) ---\n", tag, pa_off);
	fill_desc(desc_buf, data_pa, pa_off);
	log_line("desc_pa=0x%llx data_pa=0x%llx len=%u\n",
		 (unsigned long long)desc_pa, (unsigned long long)data_pa, data_size);
	log_line("desc[+0]=0x%08x [+4]=0x%08x [+8]=0x%016llx [+0x18]=0\n",
		 *(u32 *)desc_buf, ((u32 *)desc_buf)[1],
		 (unsigned long long)*(u64 *)(desc_buf + 8));

	dump_bar0("PRE-SUBMIT", bar0);

	/* the verified sequence */
	writel(0, bar1 + 4);		/* clear doorbell */
	writel(lower_32_bits(desc_pa), bar1 + 0x4080);
	writel(upper_32_bits(desc_pa), bar1 + 0x4084);
	writel(0, bar1 + 0x4088);	/* local_val: unknown semantics, try 0 */
	log_line("staged: +0x4080=0x%08x +0x4084=0x%08x +0x4088=0x%08x\n",
		 readl(bar1 + 0x4080), readl(bar1 + 0x4084), readl(bar1 + 0x4088));

	writel(0xffe7f, bar1 + 4);	/* RING */
	log_line("doorbell +4 = 0xFFE7F rung\n");

	/* poll completion: BAR1[+0x2044] bit0=valid */
	for (int i = 0; i < poll_ms * 10; i++) {	/* ~poll_ms at 100us steps */
		status = readl(bar1 + 0x2044);
		if (status & 1) break;
		udelay(100);
	}

	log_line("--- COMPLETION %s ---\n", tag);
	log_line("status(+0x2044) = 0x%08x %s\n", status,
		 (status & 1) ? "VALID" : "(timeout — no completion in poll window)");
	if (status & 1) {
		u32 data = readl(bar1 + 0x44);
		u32 wordb = readl(bar1 + 0x48);	/* read-to-clear */
		log_line("data(+0x44)    = 0x%08x (bit0=%u %s)\n", data, data & 1,
			 (data & 1) ? "= handler skips per-frame processing"
				    : "= normal completion path");
		log_line("wordB(+0x48)   = 0x%08x\n", wordb);
		writel(status, bar1 + 0x2018);	/* ack write-back (@0x63ac) */
		writel(status, bar1 + 0x2014);	/* second ack write (@0x666c) */
		log_line("acked: +0x2018=0x%08x +0x2014=0x%08x, post-ack status = 0x%08x\n",
			 readl(bar1 + 0x2018), readl(bar1 + 0x2014),
			 readl(bar1 + 0x2044));
	}

	dump_bar0("POST-SUBMIT", bar0);
	return status;
}

/* Forward refs (defined below in the Phase 3k section). */
static void enable_decoder(void __iomem *bar0, u32 hdr_off);
static void init_writer(void __iomem *bar1, void __iomem *bar0);
static int poll_ready(void __iomem *bar0, u32 hdr_off);

/* Build a multi-segment descriptor chain in desc_buf, kext-exact (arm64e
 * chain-builder @0xa0f0-0xa164): non-final desc i of N carries ctrl
 * 0xAD4B0000 | ((N-2-i)<<8), the final one 0xAD4B0013; [+4]=seg len,
 * [+8]=segment data PA (send slot — the kext's user_data arg would pick +0x10),
 * [+0x18]=64-bit PA of next desc, 0 on the last. Returns N or 0 if bad. */
static u32 build_chain(u8 *desc_buf, dma_addr_t desc_pa, dma_addr_t data_pa,
		       u32 total_len, u32 seg_size)
{
	u32 n = (total_len + seg_size - 1) / seg_size;
	u32 off = 0;

	if (!n || n > 128)	/* desc_buf holds 4096/32 descriptors */
		return 0;
	memset(desc_buf, 0, 4096);
	for (u32 i = 0; i < n; i++) {
		u32 *d = (u32 *)(desc_buf + i * 32);
		u32 len = min(seg_size, total_len - off);

		d[0] = (i == n - 1) ? 0xAD4B0013 : (0xAD4B0000 | ((n - 2 - i) << 8));
		d[1] = len;			/* [+4] segment length in bytes */
		*(u64 *)(desc_buf + i * 32 + 8) = data_pa + off;	/* [+8] PA */
		if (i < n - 1)			/* [+0x18] next-desc PA, 64-bit store */
			*(u64 *)(desc_buf + i * 32 + 0x18) = desc_pa + (i + 1) * 32;
		else
			*(u64 *)(desc_buf + i * 32 + 0x18) = 0;	/* eoc */
		log_line("chain[%u]: ctrl=0x%08x len=%-6u data_pa_off=+0x%x %s\n",
			 i, d[0], len, off, (i == n - 1) ? "(final)" : "");
		off += len;
	}
	return n;
}

/* Stage a descriptor or chain head into the given bank and ring the matching
 * doorbell, then poll completion kext-handler-style: status(+0x2044) bit0=valid,
 * count=[31:1]; count==0 -> data at +0x44 (+0x48 read-to-clear), count>=1 ->
 * batch data at +0x1044 (+0x1048 read-to-clear); dual-ack to +0x2018/+0x2014. */
static u32 do_submit_bank(const char *tag, void __iomem *bar1, void __iomem *bar0,
			  dma_addr_t head_pa, u32 bank_off, int batch)
{
	u32 status = 0;

	log_line("\n--- SUBMIT %s (bank +0x%04x%s) ---\n", tag, bank_off,
		 batch ? ", BATCH doorbell" : "");
	if (!batch) {
		writel(0, bar1 + 4);		/* clear send doorbell */
	} else {
		writel(0, bar1 + 0x1004);	/* clear recv/batch doorbell */
	}
	writel(lower_32_bits(head_pa), bar1 + bank_off);
	writel(upper_32_bits(head_pa), bar1 + bank_off + 4);
	writel(0, bar1 + bank_off + 8);	/* local_val: unknown semantics, try 0 */
	log_line("staged: +0x%04x=0x%08x +0x%04x=0x%08x +0x%04x=0x%08x\n",
		 bank_off, readl(bar1 + bank_off), bank_off + 4,
		 readl(bar1 + bank_off + 4), bank_off + 8,
		 readl(bar1 + bank_off + 8));

	if (!batch) {
		writel(0xffe7f, bar1 + 4);	/* RING send */
		log_line("doorbell +4 = 0xFFE7F rung\n");
	} else {
		writel(0x80ffe7f, bar1 + 0x1004);	/* RING batch (bit31 set) */
		log_line("doorbell +0x1004 = 0x80FFE7F rung\n");
	}

	for (int i = 0; i < poll_ms * 10; i++) {	/* ~poll_ms at 100us steps */
		status = readl(bar1 + 0x2044);
		if (status & 1) break;
		udelay(100);
	}

	log_line("--- COMPLETION %s ---\n", tag);
	log_line("status(+0x2044) = 0x%08x %s\n", status,
		 (status & 1) ? "VALID" : "(timeout — no completion in poll window)");
	if (status & 1) {
		u32 count = status >> 1;

		log_line("count(+0x2044>>1) = %u\n", count);
		if (!count) {			/* single-shot path */
			u32 data = readl(bar1 + 0x44);
			u32 wordb = readl(bar1 + 0x48);	/* read-to-clear */

			log_line("data(+0x44)    = 0x%08x (bit0=%u %s)\n", data,
				 data & 1,
				 (data & 1) ? "= handler skips per-frame processing"
					    : "= normal completion path");
			log_line("wordB(+0x48)   = 0x%08x\n", wordb);
		} else {				/* batch path */
			u32 data = readl(bar1 + 0x1044);
			u32 wordb = readl(bar1 + 0x1048);	/* read-to-clear */

			log_line("batch data(+0x1044) = 0x%08x (bit0=%u)\n", data,
				 data & 1);
			log_line("batch wordB(+0x1048)= 0x%08x\n", wordb);
		}
		writel(status, bar1 + 0x2018);	/* ack write-back (@0x63ac) */
		writel(status, bar1 + 0x2014);	/* second ack write (@0x666c) */
		log_line("acked: +0x2018=0x%08x +0x2014=0x%08x, post-ack status = 0x%08x\n",
			 readl(bar1 + 0x2018), readl(bar1 + 0x2014),
			 readl(bar1 + 0x2044));
	}

	dump_bar0("POST-SUBMIT", bar0);
	return status;
}

/* One kext-exact completion event for a non-zero status word (fn @0x5aec):
 * ack#1 -> branch on count=s>>1 -> data read (R-to-C) -> ack#2. Returns s. */
static u32 kext_handle_completion(void __iomem *bar1, void __iomem *bar0,
				  u32 hdr_off, u32 s)
{
	u32 count = s >> 1;

	writel(s, bar1 + 0x2018);	/* ack#1 (@0x63ac), before data */
	log_line("COMPLETION: status=0x%08x count=%u — kext branch:\n", s, count);
	if (!count) {
		u32 data = readl(bar1 + 0x44);
		u32 wordb = readl(bar1 + 0x48);	/* R-to-C */

		log_line("  single-shot: data(+0x44)=0x%08x (bit0=%u) wordB(+0x48)=0x%08x\n",
			 data, data & 1, wordb);
	} else if (count == 1) {
		u32 h10 = readl(bar0 + hdr_off + 0x10);
		u32 idx = h10 >> 16;
		u32 data = readl(bar1 + 0x1044);
		u32 wordb = readl(bar1 + 0x1048);	/* R-to-C */

		log_line("  batch: BAR0[hdr+0x10]=0x%08x idx(>>16)=%u\n", h10, idx);
		log_line("         data(+0x1044)=0x%08x (bit0=%u) wordB(+0x1048)=0x%08x\n",
			 data, data & 1, wordb);
	} else {
		log_line("  count>1: no per-item read (kext @0x6568)\n");
	}
	writel(s, bar1 + 0x2014);	/* ack#2 (@0x666c), after data */
	log_line("  acked: +0x2018=0x%08x +0x2014=0x%08x post-ack status=0x%08x\n",
		 readl(bar1 + 0x2018), readl(bar1 + 0x2014),
		 readl(bar1 + 0x2044));
	return s;
}

/* Kext-exact completion handling per fn @0x5aec (byte-verified):
 *   w25 = BAR1[+0x2044];  str w25, [+0x2018]      ; ack#1 BEFORE data read
 *   count = w25 >> 1:
 *     count==0 -> data=BAR1[+0x44],  R-to-C BAR1[+0x48]
 *     count==1 -> idx=[BAR0 hdr+0x10]>>16, data=BAR1[+0x1044], R-to-C [+0x1048]
 *     count>1  -> no per-item read (w26=0)
 *   str w25, [+0x2014]                            ; ack#2 AFTER data read
 * NOTE: there is NO bit0 gate — any non-zero status is a completion event.
 * Polls at ~5ms steps for poll_ms, logging every status change (trajectory);
 * each distinct non-zero value is handled once. Returns last handled status. */
static u32 do_submit_kext(const char *tag, void __iomem *bar1, void __iomem *bar0,
			  dma_addr_t head_pa, u32 bank_off, int batch, u32 hdr_off)
{
	u32 last = 0;

	log_line("\n--- SUBMIT %s (bank +0x%04x%s) — kext-exact completion ---\n", tag,
		 bank_off, batch ? ", BATCH doorbell" : "");

	/* Drain any pending completions BEFORE submitting: the engine ignores a
	 * new doorbell while an un-acked completion sits in +0x2044 (mode=10 run 1:
	 * stale R9-2 status=0x2 present, chain not consumed, +0x2010 stayed 3). */
	for (int d = 0; d < 50; d++) {
		u32 s = readl(bar1 + 0x2044);

		if (!s)
			break;
		log_line("pre-submit drain: pending status=0x%08x — handling\n", s);
		kext_handle_completion(bar1, bar0, hdr_off, s);
		mdelay(5);
	}

	if (!batch) {
		writel(0, bar1 + 4);
	} else {
		writel(0, bar1 + 0x1004);
	}
	writel(lower_32_bits(head_pa), bar1 + bank_off);
	writel(upper_32_bits(head_pa), bar1 + bank_off + 4);
	writel(0, bar1 + bank_off + 8);
	log_line("staged: +0x%04x=0x%08x +0x%04x=0x%08x +0x%04x=0x%08x\n",
		 bank_off, readl(bar1 + bank_off), bank_off + 4,
		 readl(bar1 + bank_off + 4), bank_off + 8,
		 readl(bar1 + bank_off + 8));

	if (!batch) {
		writel(0xffe7f, bar1 + 4);
		log_line("doorbell +4 = 0xFFE7F rung\n");
	} else {
		writel(0x80ffe7f, bar1 + 0x1004);
		log_line("doorbell +0x1004 = 0x80FFE7F rung\n");
	}

	u32 eng_before = readl(bar1 + 0x2010);
	u32 prev = readl(bar1 + 0x2044);
	u32 last_handled = 0;

	log_line("pre-poll: status=0x%08x eng_ctrl(+0x2010)=%u\n", prev, eng_before);

	for (int i = 0; i < poll_ms / 5 + 1; i++) {
		u32 s = readl(bar1 + 0x2044);

		if (s != prev) {
			log_line("t=%+5d ms: status(+0x2044) 0x%08x -> 0x%08x\n",
				 i * 5, prev, s);
			prev = s;
		}
		if (s && s != last_handled) {
			kext_handle_completion(bar1, bar0, hdr_off, s);
			last_handled = s;
			last = s;
		}
		mdelay(5);
	}

	log_line("post-poll: status=0x%08x eng_ctrl(+0x2010) %u -> %u\n", prev,
		 eng_before, readl(bar1 + 0x2010));

	/* decoder error word — kext-exact R-to-C (@0x63f8/0x63fc): read + write-back */
	u32 err = readl(bar0 + hdr_off + 0x14);

	writel(err, bar0 + hdr_off + 0x14);
	log_line("decoder status BAR0[hdr+0x14]=0x%08x (low5=0x%x %s) — read+write-back done\n",
		 err, err & 0x1f,
		 (err & 0x1f) ? "KEXT WOULD LOG 'Error from decoder'" : "clean");

	dump_bar0("POST-SUBMIT", bar0);
	return last;
}

/* Mode 11: long instrumented poll — 10ms steps for poll_ms, logging every
 * status(+0x2044) or decoder-counter(BAR0[hdr+0x14]) change plus a heartbeat
 * every ~5s. Handles each rising edge (zero -> non-zero) kext-exact; a stuck
 * non-zero value is NOT re-acked (avoids ack spam). Returns last handled
 * status word, 0 if none. */
static u32 m11_poll(const char *tag, void __iomem *bar1, void __iomem *bar0,
		    u32 hdr_off)
{
	u32 last = 0;
	u32 prev_s = readl(bar1 + 0x2044);
	u32 prev_c = readl(bar0 + hdr_off + 0x14);
	int edge_armed = !prev_s;	/* can detect a rising edge if zero now */

	for (int i = 0; i < poll_ms / 10 + 1; i++) {
		u32 s = readl(bar1 + 0x2044);
		u32 c = readl(bar0 + hdr_off + 0x14);

		if (s != prev_s || c != prev_c) {
			log_line("%s t=%+6d ms: status=0x%08x ctr(+0x8014)=0x%08x\n",
				 tag, i * 10, s, c);
		} else if (i && i % 500 == 250) {
			log_line("%s t=%+6d ms: ... status=0x%08x ctr=0x%08x (no change)\n",
				 tag, i * 10, s, c);
		}

		if (s && edge_armed) {	/* rising edge = new completion event */
			kext_handle_completion(bar1, bar0, hdr_off, s);
			last = s;
			prev_s = readl(bar1 + 0x2044);	/* re-read post-ack */
		} else {
			prev_s = s;
		}
		edge_armed = !prev_s;
		prev_c = c;
		mdelay(10);
	}
	return last;
}

/* Mode 11/12: drain pending events kext-exact, build a seg_size-segmented
 * chain, stage it in the given bank (bank_off, local_val=0), ring ring_val at
 * db_off and poll with m11_poll. Returns last handled status word
 * (0 = no completion in window). */
static u32 m11_submit(const char *tag, void __iomem *bar1, void __iomem *bar0,
		      u32 hdr_off, u8 *desc_buf, dma_addr_t desc_pa,
		      dma_addr_t data_pa, u32 data_size, u32 seg_size,
		      u32 bank_off, u32 db_off, u32 ring_val)
{
	for (int d = 0; d < 50; d++) {
		u32 s = readl(bar1 + 0x2044);

		if (!s)
			break;
		log_line("%s pre-submit drain: pending status=0x%08x — handling\n", tag, s);
		kext_handle_completion(bar1, bar0, hdr_off, s);
		mdelay(5);
	}

	u32 n = build_chain(desc_buf, desc_pa, data_pa, data_size, seg_size);

	if (!n) {
		log_line("%s chain build FAILED (bad size)\n", tag);
		return 0;
	}

	writel(0x1f, bar0 + hdr_off + 0x18);	/* kext-exact pre-submit write */
	writel(0, bar1 + db_off);			/* clear doorbell */
	writel(lower_32_bits(desc_pa), bar1 + bank_off);
	writel(upper_32_bits(desc_pa), bar1 + bank_off + 4);
	writel(0, bar1 + bank_off + 8);		/* local_val = 0 (isolate N) */

	/* last-chance drain immediately pre-ring (event may have arrived late) */
	for (int d = 0; d < 20; d++) {
		u32 s = readl(bar1 + 0x2044);

		if (!s)
			break;
		log_line("%s late drain: pending status=0x%08x — handling\n", tag, s);
		kext_handle_completion(bar1, bar0, hdr_off, s);
		mdelay(5);
	}

	log_line("%s staged: N=%u seg=0x%x head=0x%llx\n", tag, n, seg_size,
		 (unsigned long long)desc_pa);

	u32 t0 = jiffies;

	writel(ring_val, bar1 + db_off);	/* RING */
	log_line("%s doorbell +0x%x = 0x%05X rung\n", tag, db_off, ring_val);

	u32 s = m11_poll(tag, bar1, bar0, hdr_off);

	if (s)
		log_line("%s COMPLETED after %u ms (status=0x%08x)\n", tag,
			 jiffies_to_msecs(jiffies - t0), s);
	else
		log_line("%s NO completion in %d ms window\n", tag, poll_ms);
	return s;
}

/* Re-run the wedge-recovery cycle found in mode=8 rung A: decoder disable/enable
 * + init writer + ready poll. Returns ready bit. */
static int rebring(void __iomem *bar1, void __iomem *bar0, u32 hdr_off)
{
	enable_decoder(bar0, hdr_off);
	init_writer(bar1, bar0);
	return poll_ready(bar0, hdr_off);
}

/* ---- Phase 3k: decoder bring-up (mirrors kext runtime fn @0x7508) ---- */

/* Firmware version gate (@0x75c4-0x78ac): word at BAR0+0x8, upper16 must be 2,
 * lower16 in {0x3E, 0x3F}. Returns 1 if the kext would proceed. */
static int fw_version_gate(void __iomem *bar0)
{
	u32 v = readl(bar0 + 0x8);

	log_line("\n--- FW VERSION GATE (@0x75c4) ---\n");
	log_line("BAR0+0x0008 = 0x%08x (upper=0x%04x lower=0x%04x)\n", v,
		 (v >> 16) & 0xffff, v & 0xffff);
	if (((v >> 16) & 0xffff) != 2) {
		log_line("GATE FAIL: upper half != 2 — kext would log @0x656 and stop\n");
		return 0;
	}
	if ((v & 0xffff) != 0x3e && (v & 0xffff) != 0x3f) {
		log_line("GATE FAIL: lower half not in {0x3E,0x3F} — kext would stop\n");
		return 0;
	}
	log_line("GATE PASS (kext @0x7820/0x789c accepts this firmware)\n");
	return 1;
}

/* Walk the section directory (@0x5d14-0x5e00 / copy @0x7918-0x7990):
 * entry W at offset O: type = W&0xFF (0 terminates), next = O + ((W>>14)&0x3FFFC).
 * Returns the decoder header offset (first type==1) or 0 if not found.
 * Capped at 16 iterations as a safety net; on the live card the walk is
 * well-formed (type-1 entry stride=0x2C, next word at +0x802C = 0 terminates). */
static u32 walk_directory(void __iomem *bar0)
{
	u32 off = 0;

	log_line("\n--- SECTION DIRECTORY WALK (@0x7918) ---\n");
	for (int i = 0; i < 16; i++) {
		u32 w = readl(bar0 + off);
		u32 type = w & 0xff;
		u32 stride = (w >> 14) & 0x3fffc;

		log_line("  [%d] BAR0+0x%05x word=0x%08x type=%u stride=0x%x\n",
			 i, off, w, type, stride);
		if (type == 1) {
			log_line("  DECODER section header found at BAR0+0x%05x\n", off);
			return off;
		}
		if (type == 0 || stride == 0) break;
		off += stride;
	}
	log_line("  no type==1 entry found (or walk terminated)\n");
	return 0;
}

/* Disable/enable the Decoder Control Register (@0x79f8-0x7a64): hdr+4 = 0,
 * delay ~1ms (kext bl #0xe89c w0=1), hdr+4 = 1. */
static void enable_decoder(void __iomem *bar0, u32 hdr_off)
{
	log_line("\n--- DECODER CONTROL (@0x79f8-0x7a64) ---\n");
	log_line("pre: BAR0+0x%05x (hdr+4) = 0x%08x\n", hdr_off + 4, readl(bar0 + hdr_off + 4));
	writel(0, bar0 + hdr_off + 4);	/* "(disabling)" */
	log_line("disable: wrote 0 -> readback 0x%08x\n", readl(bar0 + hdr_off + 4));
	usleep_range(2000, 5000);
	writel(1, bar0 + hdr_off + 4);	/* "(enabling)" */
	log_line("enable : wrote 1 -> readback 0x%08x\n", readl(bar0 + hdr_off + 4));
}

/* Full init writer @0x5a3c-0x5ad0, kext-exact (BAR1 words + BAR0 magic region
 * + PR block reset). Logs before/after for every BAR0 write. */
static void init_writer(void __iomem *bar1, void __iomem *bar0)
{
	log_line("\n--- INIT WRITER (@0x5a3c-0x5ad0, kext-exact) ---\n");

	writel(0xfffffe, bar1 + 0x90);
	writel(0xfffffe, bar1 + 0x1090);
	writel(0x3, bar1 + 0x2010);
	log_line("BAR1: +0x90=0x%08x +0x1090=0x%08x +0x2010=0x%08x\n",
		 readl(bar1 + 0x90), readl(bar1 + 0x1090), readl(bar1 + 0x2010));

	/* magic region (BAR0+0x1040..6C) — idempotent on this card per Phase 0 */
	struct { u32 off; u32 val; } mags[] = {
		{ 0x1040, 0x2D002A80 }, { 0x1048, 0x2D002A80 },
		{ 0x104C, 0x2A802800 }, { 0x1050, 0x2D002A80 },
		{ 0x1054, 0xBCD3BA4D }, { 0x1058, 0x2F802D00 },
		{ 0x1060, 0x2F802D00 }, { 0x1064, 0x2D002A80 },
		{ 0x1068, 0x2F802D00 }, { 0x106C, 0xBF58BCD3 },
	};
	for (unsigned i = 0; i < ARRAY_SIZE(mags); i++) {
		u32 before = readl(bar0 + mags[i].off);
		writel(mags[i].val, bar0 + mags[i].off);
		log_line("BAR0+0x%04x: 0x%08x -> wrote 0x%08x (rb 0x%08x)%s\n",
			 mags[i].off, before, mags[i].val, readl(bar0 + mags[i].off),
			 (before == mags[i].val) ? " [no-op]" : "");
	}

	/* the two that DO change on this card (Phase 0: both were 0x3FFF) */
	for (u32 o = 0x1030; o <= 0x1034; o += 4) {
		u32 before = readl(bar0 + o);
		writel(0x7d, bar0 + o);
		log_line("BAR0+0x%04x: 0x%08x -> wrote 0x007d (rb 0x%08x)\n",
			 o, before, readl(bar0 + o));
	}

	/* PR block reset (Phase 0: +0x202C=7, +0x2030=7) */
	struct { u32 off; u32 val; } pr[] = {
		{ 0x202C, 0 }, { 0x2030, 6 }, { 0x2034, 0 },
	};
	for (unsigned i = 0; i < ARRAY_SIZE(pr); i++) {
		u32 before = readl(bar0 + pr[i].off);
		writel(pr[i].val, bar0 + pr[i].off);
		log_line("BAR0+0x%04x: 0x%08x -> wrote 0x%08x (rb 0x%08x)\n",
			 pr[i].off, before, pr[i].val, readl(bar0 + pr[i].off));
	}

	writel(0x16, bar1 + 0x2004);
	log_line("BAR1: +0x2004 = 0x%08x (wrote 0x16)\n", readl(bar1 + 0x2004));
}

/* Poll the readiness bit: frame-submit path @~0xa700 tests bit0 of BAR0[hdr+8]
 * and logs "Decoder not ready!! (0x%08x)" when clear. Up to ~2s. */
static int poll_ready(void __iomem *bar0, u32 hdr_off)
{
	u32 r;

	log_line("\n--- READINESS POLL (bit0 of BAR0+0x%05x) ---\n", hdr_off + 8);
	for (int i = 0; i < 200; i++) {
		r = readl(bar0 + hdr_off + 8);
		if (r & 1) break;
		usleep_range(9000, 11000);
	}
	log_line("BAR0+0x%05x = 0x%08x -> %s\n", hdr_off + 8, r,
		 (r & 1) ? "READY" : "NOT READY after ~2s");
	return r & 1;
}

/* One rung of the mode-8 recovery ladder: submit len=64 and report state. */
static u32 r8_try(const char *name, void __iomem *bar1, void __iomem *bar0,
		  u8 *desc_buf, dma_addr_t desc_pa, dma_addr_t data_pa)
{
	u32 s = do_submit(name, bar1, bar0, desc_buf, desc_pa, data_pa, 8);

	log_line("R8 %-36s -> %s (eng ctrl=%u +0x2018=0x%08x +0x2014=0x%08x)\n",
		 name, (s & 1) ? "CONSUMED" : "timeout",
		 readl(bar1 + 0x2010), readl(bar1 + 0x2018), readl(bar1 + 0x2014));
	return s;
}

/* BAR1-only engine init (@0x5a3c-0x5acc subset — the four BAR1 words). */
static void bar1_init(void __iomem *bar1)
{
	log_line("\n--- INIT (BAR1-only, from @0x5a3c-0x5acc) ---\n");
	writel(0xfffffe, bar1 + 0x90);
	writel(0xfffffe, bar1 + 0x1090);
	writel(0x3, bar1 + 0x2010);
	log_line("  +0x90   readback = 0x%08x\n", readl(bar1 + 0x90));
	log_line("  +0x1090 readback = 0x%08x\n", readl(bar1 + 0x1090));
	log_line("  +0x2010 readback = 0x%08x\n", readl(bar1 + 0x2010));
	writel(0x16, bar1 + 0x2004);
	log_line("  +0x2004 readback = 0x%08x (wrote 0x16)\n", readl(bar1 + 0x2004));
}

static int ab_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	void __iomem *bar1, *bar0 = NULL;
	u32 size;
	dma_addr_t desc_pa, data_pa;
	u8 *desc_buf, *data_buf;
	const u32 DESC_SIZE = 4096;

	log_line("Apple Afterburner [106b:0205] Phase 3i-3k BAR1 submit + bring-up (mode=%d)\n\n", mode);

	if (pci_enable_device(pdev)) { log_line("pci_enable_device FAILED\n"); return -ENODEV; }
	pci_set_master(pdev);
	if (pci_request_regions(pdev, "afterburner_submit2")) {
		log_line("request_regions FAILED\n"); goto out_disable;
	}

	size = pci_resource_len(pdev, 1);
	bar1 = pci_iomap(pdev, 1, size);
	if (!bar1) { log_line("iomap BAR1 FAILED\n"); goto out_release; }

	bar0 = pci_iomap(pdev, 0, 0);	/* full BAR0 (observables + bring-up writes in mode 4/5) */

	dump_regs("PRE-STATE", bar1);
	if (bar0) dump_bar0("PRE-STATE", bar0);

	if (mode == 0) goto done;

	/* ---- Phase 3k: full decoder bring-up (modes 4, 5, 6, 8, 9 and 10) ---- */
	u32 hdr_off = 0;
	int ready = 0;
	if (mode == 4 || mode == 5 || mode == 6 || mode == 8 || mode == 9 || mode == 10 || mode == 11 || mode == 12) {
		if (!bar0) { log_line("BAR0 map missing — cannot bring up\n"); goto done; }

		if (!fw_version_gate(bar0)) goto done;

		hdr_off = walk_directory(bar0);
		if (!hdr_off) {
			log_line("\n--- BRING-UP ABORTED: no decoder section (kext @0x7b34 path) ---\n");
			goto done;
		}
		dump_channel("PRE-ENABLE", bar0, hdr_off);

		enable_decoder(bar0, hdr_off);
		init_writer(bar1, bar0);
		ready = poll_ready(bar0, hdr_off);

		dump_channel("POST-BRINGUP", bar0, hdr_off);
	}

	/* ---- legacy BAR1-only init (modes 1, 3 and 7) ---- */
	if (mode == 1 || mode == 3 || mode == 7) {
		bar1_init(bar1);
	}

	/* ---- allocate coherent DMA: descriptor chain + data buffer ---- */
	int use_file = 0;
	u32 payload_len = data_size;

	if (mode == 7)
		payload_len = SWEEP_MAX;	/* one allocation covers the whole sweep */
	else if ((mode == 9 || mode == 10 || mode == 11 || mode == 12) && !frame_path[0])
		payload_len = SWEEP_MAX;	/* pattern fallback sized like the real frame */

	if ((mode == 6 || mode == 9 || mode == 10 || mode == 11 || mode == 12) && frame_path[0]) {
		struct file *f = filp_open(frame_path, O_RDONLY, 0);

		if (!IS_ERR(f)) {
			loff_t sz = i_size_read(file_inode(f));

			filp_close(f, NULL);
			if (sz > 0 && sz <= MAX_FRAME_SIZE) {
				payload_len = (u32)sz;
				use_file = 1;
			} else {
				log_line("frame file %s: bad size (%lld, max %u)\n",
					 frame_path, (long long)sz, MAX_FRAME_SIZE);
			}
		} else {
			log_line("frame file %s: open FAILED\n", frame_path);
		}
	}

	desc_buf = dma_alloc_coherent(&pdev->dev, DESC_SIZE, &desc_pa, GFP_KERNEL);
	data_buf = dma_alloc_coherent(&pdev->dev, payload_len, &data_pa, GFP_KERNEL);
	if (!desc_buf || !data_buf) { log_line("DMA alloc FAILED\n"); goto done; }

	if (use_file) {
		struct file *f = filp_open(frame_path, O_RDONLY, 0);
		loff_t pos = 0;
		int n;

		if (IS_ERR(f)) {
			log_line("frame file re-open FAILED\n");
			use_file = 0;
		} else {
			n = kernel_read(f, data_buf, payload_len, &pos);
			filp_close(f, NULL);
			if (n != payload_len) {
				log_line("frame read short: %d/%u\n", n, payload_len);
				use_file = 0;
			} else {
				data_size = payload_len;	/* desc[+4] = real frame length */
				log_line("loaded ProRes sample from %s: %u bytes (head=%02x%02x%02x%02x magic@+4=%c%c%c%c)\n",
					 frame_path, payload_len,
					 data_buf[0], data_buf[1], data_buf[2], data_buf[3],
					 data_buf[4], data_buf[5], data_buf[6], data_buf[7]);
			}
		}
	}

	if (!use_file) {
		/* payload: recognizable pattern (fill the whole allocation so any
		 * sweep size sees valid data in its prefix) */
		for (u32 i = 0; i < payload_len / 4; i++)
			((u32 *)data_buf)[i] = 0xAB5A0000 + i;
	}

	if (mode == 1 || mode == 2) {
		do_submit("send", bar1, bar0, desc_buf, desc_pa, data_pa, 8);
	} else if (mode == 3) {
		/* Dual-variant test: kext-exact send layout first (PA@+0x08),
		 * then the recv-slot variant (PA@+0x10). */
		u32 sA = do_submit("A: kext-exact", bar1, bar0,
				   desc_buf, desc_pa, data_pa, 8);
		if (sA & 1)
			do_submit("B: PA@+0x10 variant", bar1, bar0,
				  desc_buf, desc_pa, data_pa, 0x10);
		else
			log_line("\n--- SUBMIT B skipped (A did not complete) ---\n");
	} else if (mode == 5) {
		if (ready) {
			u32 s = do_submit("send (decoder READY)", bar1, bar0,
					  desc_buf, desc_pa, data_pa, 8);
			log_line("\n--- MODE-5 VERDICT ---\n");
			log_line("completion=%s; compare data(+0x44) above vs 0x6 from modes 1/3\n",
				 (s & 1) ? "VALID" : "TIMEOUT");
		} else {
			log_line("\n--- MODE-5: submit SKIPPED — decoder not ready after bring-up ---\n");
		}
	} else if (mode == 6) {
		if (!ready) {
			log_line("\n--- MODE-6: submit SKIPPED — decoder not ready after bring-up ---\n");
		} else {
			u32 before = readl(bar0 + hdr_off + 0x18);

			/* kext-exact pre-submit write (@0xa714): when the ready bit is
			 * set, BAR0[hdr+0x18] = 0x1F goes out right before the frame. */
			writel(0x1f, bar0 + hdr_off + 0x18);
			log_line("\n--- MODE-6 PRE-SUBMIT WRITE (@0xa714) ---\n");
			log_line("BAR0+0x%05x (hdr+0x18): 0x%08x -> wrote 0x1f (rb 0x%08x)\n",
				 hdr_off + 0x18, before, readl(bar0 + hdr_off + 0x18));

			{
				u32 s = do_submit(use_file ? "send REAL ProRes frame" : "send (pattern fallback)",
						  bar1, bar0, desc_buf, desc_pa, data_pa, 8);

				log_line("\n--- MODE-6 VERDICT ---\n");
				log_line("completion=%s payload=%u bytes (%s)\n",
					 (s & 1) ? "VALID" : "TIMEOUT", data_size,
					 use_file ? "real ProRes sample" : "pattern fallback");
				log_line("watch: data(+0x44) vs 0x6 from modes 1/3/5; BAR0+0x8014 counter\n");
			}
		}
	} else if (mode == 7) {
		/* ---- Phase 3m: single-shot length sweep ---- */
		log_line("\n--- MODE-7 LENGTH SWEEP (single-shot, pattern payload) ---\n");
		if (bar0) {
			u32 rdy = readl(bar0 + 0x8008);

			log_line("decoder state: BAR0+0x8004=0x%08x ready(+0x8008)=0x%08x (bit0=%u)\n",
				 readl(bar0 + 0x8004), rdy, !!(rdy & 1));
		}
		for (unsigned i = 0; i < ARRAY_SIZE(sweep_sizes); i++) {
			u32 n = sweep_sizes[i];
			u32 eng_before = readl(bar1 + 0x2010);

			data_size = n;	/* fill_desc reads the global */
			{
				u32 s = do_submit("sweep", bar1, bar0, desc_buf, desc_pa, data_pa, 8);
				u32 eng_after = readl(bar1 + 0x2010);

				log_line("SWEEP len=%-6u -> %s (eng ctrl %u->%u)\n", n,
						(s & 1) ? "CONSUMED" : "NOT consumed (timeout)",
						eng_before, eng_after);
			}
		}
		log_line("\n--- MODE-7 VERDICT ---\n");
		log_line("largest CONSUMED len above = single-shot max; first NOT-consumed = ceiling\n");
	} else if (mode == 8) {
		/* ---- Phase 3n: recovery matrix — find what un-wedges the engine ---- */
		log_line("\n--- MODE-8 RECOVERY MATRIX (ladder until len=64 is consumed) ---\n");

		data_size = 64;
		if (!r8_try("A: as-is after bring-up", bar1, bar0, desc_buf, desc_pa, data_pa)) {
			u32 a18 = readl(bar1 + 0x2018);

			writel(0, bar1 + 0x2018);
			log_line("R8 cleared +0x2018 (was 0x%08x)\n", a18);
			if (!r8_try("B: after clear +0x2018", bar1, bar0, desc_buf, desc_pa, data_pa)) {
				u32 a14 = readl(bar1 + 0x2014);

				writel(0, bar1 + 0x2014);
				log_line("R8 cleared +0x2014 (was 0x%08x)\n", a14);
				if (!r8_try("C: after clear both ack regs", bar1, bar0, desc_buf, desc_pa, data_pa)) {
					int r = pci_reset_function(pdev);

					log_line("R8 PCIe FLR (pci_reset_function): %s\n",
							r ? "FAILED" : "ok");
					if (!r) {
						bar1_init(bar1);
						if (!r8_try("D: after FLR + re-init", bar1, bar0, desc_buf, desc_pa, data_pa))
							log_line("\n--- MODE-8 VERDICT: still wedged — needs power cycle ---\n");
					} else {
						log_line("\n--- MODE-8 VERDICT: no FLR; ack clears failed ---\n");
					}
				}
			}
		}
		log_line("first CONSUMED rung above = the recovery that works\n");
	} else if (mode == 9) {
	/* ---- Phase 3o: chain-submit ladder — kext-exact multi-segment chains ---- */
	if (!ready) {
		log_line("\n--- MODE-9: submit SKIPPED — decoder not ready after bring-up ---\n");
	} else {
		u32 nseg = build_chain(desc_buf, desc_pa, data_pa, data_size, 4096);

		log_line("\n--- MODE-9 CHAIN LADDER (payload=%u bytes%s) ---\n",
			 data_size, use_file ? ", real ProRes frame" : ", pattern");
		if (!nseg) {
			log_line("chain build FAILED (bad size)\n");
		} else {
			u32 before = readl(bar0 + hdr_off + 0x18);

			/* kext-exact pre-submit write (@0xa714), as in mode 6 */
			writel(0x1f, bar0 + hdr_off + 0x18);
			log_line("pre-submit: BAR0+0x%05x (hdr+0x18): 0x%08x -> wrote 0x1f (rb 0x%08x)\n",
				 hdr_off + 0x18, before, readl(bar0 + hdr_off + 0x18));

			/* Rung 1: chain head into BATCH bank (+0x5080), ring 0x80FFE7F —
			 * the kext's frame path commits via vtable[+0x150]; §7.4 flag=1 = batch */
			if (!do_submit_bank("R9-1 chain->batch bank", bar1, bar0, desc_pa, 0x5080, 1)) {
				log_line("\n--- R9 rung 1 failed: re-bring-up (mode-8 recovery) ---\n");
				if (!rebring(bar1, bar0, hdr_off))
					log_line("decoder NOT ready after re-bring-up\n");

				/* Rung 2: chain head into SEND bank (+0x4080), ring 0xFFE7F —
				 * tests whether the send path walks +0x18 links */
				if (!do_submit_bank("R9-2 chain->send bank", bar1, bar0, desc_pa, 0x4080, 0)) {
					log_line("\n--- R9 rung 2 failed: re-bring-up (mode-8 recovery) ---\n");
					if (!rebring(bar1, bar0, hdr_off))
						log_line("decoder NOT ready after re-bring-up\n");

					/* Rung 3: single descriptor, full length, batch bank */
					fill_desc(desc_buf, data_pa, 8);
					if (!do_submit_bank("R9-3 single-desc->batch", bar1, bar0, desc_pa, 0x5080, 1)) {
						log_line("\n--- MODE-9 VERDICT: all rungs timed out ---\n");
						log_line("next variables: PA slot +0x10 (user_data variant), decode[+0xc] doorbell\n");
					} else {
						log_line("\n--- MODE-9 VERDICT: rung 3 (single-desc batch) completed ---\n");
					}
				} else {
					log_line("\n--- MODE-9 VERDICT: rung 2 (chain via send bank) completed ---\n");
				}
			} else {
				log_line("\n--- MODE-9 VERDICT: rung 1 (chain via batch bank) completed ---\n");
			}
		}
	}
	} else if (mode == 10) {
	/* ---- Phase 3p: kext-exact chain completion — read what count=1 means ---- */
	if (!ready) {
		log_line("\n--- MODE-10: submit SKIPPED — decoder not ready after bring-up ---\n");
	} else {
		u32 nseg = build_chain(desc_buf, desc_pa, data_pa, data_size, 4096);

		log_line("\n--- MODE-10 KEXT-EXACT CHAIN COMPLETION (payload=%u bytes%s) ---\n",
			 data_size, use_file ? ", real ProRes frame" : ", pattern");
		if (!nseg) {
			log_line("chain build FAILED (bad size)\n");
		} else {
			u32 before = readl(bar0 + hdr_off + 0x18);

			/* kext-exact pre-submit write (@0xa714), as in modes 6/9 */
			writel(0x1f, bar0 + hdr_off + 0x18);
			log_line("pre-submit: BAR0+0x%05x (hdr+0x18): 0x%08x -> wrote 0x1f (rb 0x%08x)\n",
				 hdr_off + 0x18, before, readl(bar0 + hdr_off + 0x18));

			/* R9-2 config: chain head into SEND bank (+0x4080), ring 0xFFE7F —
			 * the only rung that produced a non-zero status (count=1) in mode 9 */
			u32 s = do_submit_kext("M10 chain->send bank", bar1, bar0, desc_pa,
						0x4080, 0, hdr_off);

			log_line("\n--- MODE-10 VERDICT ---\n");
			if (s) {
				u32 count = s >> 1;

				log_line("completion status=0x%08x count=%u — data word read per kext branch above\n",
					 s, count);
				if (count == 1)
					log_line("count==1 => engine registered ONE completion for the whole chain (frame-level)\n");
			} else {
				log_line("no completion in poll window — check trajectory above\n");
			}
		}
	}
	} else if (mode == 11) {
	if (!ready) {
		log_line("\n--- MODE-11: submit SKIPPED — decoder not ready after bring-up ---\n");
	} else {
		u32 s1, s2;

		log_line("\n--- MODE-11 INSTRUMENTED DUAL CHAIN (payload=%u bytes%s) ---\n",
			 data_size, use_file ? ", real ProRes frame" : ", pattern");
		log_line("baseline: status=0x%08x ctr(+0x8014)=0x%08x\n",
			 readl(bar1 + 0x2044), readl(bar0 + hdr_off + 0x14));

		s1 = m11_submit("M11-A N=5 seg=4096 ", bar1, bar0, hdr_off, desc_buf,
				desc_pa, data_pa, data_size, 4096,
				0x4080, 4, 0xffe7f);
		if (!s1)
			log_line("WARNING: M11-A did not complete — M11-B results may be confounded\n");
		s2 = m11_submit("M11-B N=10 seg=2048", bar1, bar0, hdr_off, desc_buf,
				desc_pa, data_pa, data_size, 2048,
				0x4080, 4, 0xffe7f);

		log_line("\n--- MODE-11 VERDICT ---\n");
		if (s1 && s2)
			log_line("both chains completed — compare wordB above: N=5 vs N=10 (live per-event => engine walked each chain)\n");
		else if (s1 || s2)
			log_line("one chain completed, one did not — see trajectories\n");
		else
			log_line("neither chain completed in window — extend poll_ms or inspect drain logs\n");
	}
	} else if (mode == 12) {
	if (!ready) {
		log_line("\n--- MODE-12: submit SKIPPED — decoder not ready after bring-up ---\n");
	} else {
		u32 sA, sB, sC;

		log_line("\n--- MODE-12 BATCH-BANK CHAIN RUN (payload=%u bytes%s) ---\n",
			 data_size, use_file ? ", real ProRes frame" : ", pattern");
		log_line("baseline: status=0x%08x ctr(+0x8014)=0x%08x\n",
			 readl(bar1 + 0x2044), readl(bar0 + hdr_off + 0x14));

		sA = m11_submit("M12-A batch N=10 seg=2048", bar1, bar0, hdr_off, desc_buf,
				desc_pa, data_pa, data_size, 2048,
				0x5080, 0x1004, 0x80ffe7f);
		sB = m11_submit("M12-B batch N=5 seg=4096 ", bar1, bar0, hdr_off, desc_buf,
				desc_pa, data_pa, data_size, 4096,
				0x5080, 0x1004, 0x80ffe7f);
		sC = m11_submit("M12-C send N=5 seg=4096  ", bar1, bar0, hdr_off, desc_buf,
				desc_pa, data_pa, data_size, 4096,
				0x4080, 4, 0xffe7f);

		log_line("\n--- MODE-12 VERDICT ---\n");
		if (sA)
			log_line("M12-A (N=10 batch): completed — wordB above should be 10 if live per-event\n");
		else
			log_line("M12-A (N=10 batch): no completion in window\n");
		if (sB)
			log_line("M12-B (N=5 batch): completed — wordB above should be 5 (reproducibility)\n");
		else
			log_line("M12-B (N=5 batch): no completion in window\n");
		if (sC)
			log_line("M12-C (N=5 send): completed within window — send-bank latency measured above\n");
		else
			log_line("M12-C (N=5 send): not complete in window — check next run's drain for delayed completion\n");
	}
	}

	dump_regs("POST-STATE", bar1);

done:
	if (bar0) pci_iounmap(pdev, bar0);
	pci_iounmap(pdev, bar1);
out_release:
	pci_release_regions(pdev);
out_disable:
	pci_disable_device(pdev);
	log_line("\n=== done ===\n");
	return 0;
}

static void ab_remove(struct pci_dev *pdev)
{
	/* nothing persistent to undo */
}

static const struct pci_device_id ab_ids[] = {
	{ PCI_DEVICE(AB_VENDOR, AB_DEVICE) },
	{}
};
MODULE_DEVICE_TABLE(pci, ab_ids);

static struct pci_driver ab_driver = {
	.name = "afterburner_submit2",
	.id_table = ab_ids,
	.probe = ab_probe,
	.remove = ab_remove,
};
module_pci_driver(ab_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("afterburner-re");
MODULE_DESCRIPTION("Phase 3i-3k: BAR1 DMA submit + kext-exact decoder bring-up");
