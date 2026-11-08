/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _AB_REGS_H_
#define _AB_REGS_H_

#include <linux/types.h>

enum ab_bar { AB_BAR0, AB_BAR1 };

/* ------------------------------------------------------------------ *
 * BAR0 — control/status (FINDINGS §7–§9, §21; all live-verified)
 * ------------------------------------------------------------------ */

/* Firmware version gate word (@0x75c4): upper16 must be 2, lower16 in {0x3E,0x3F} */
#define AB_B0_FW_VERSION	0x0008

/* Decoder section header fields (relative to hdr_off from the dir walk) */
#define AB_DEC_ENABLE		4	/* 0 = disable (kext stop cmd), 1 = enable */
#define AB_DEC_READY		8	/* bit0 set = ready ("Decoder not ready!!" log) */

/* Magic region — init writer @0x5a3c, 10 words; idempotent on this card (Phase 0) */
#define AB_B0_MAGIC_BASE	0x1040	/* ..+0x6C */
#define AB_B0_MASK_LO		0x1030	/* -> 0x7D (was 0x3FFF cold) */
#define AB_B0_MASK_HI		0x1034	/* -> 0x7D (was 0x3FFF cold) */

/* PR block — init writer; +0x202C is NOT writable on this card (readback stays
 * 7) — write-and-ignore per live verification (plan §8.3 item 5). */
#define AB_B0_PR_0		0x202C	/* -> 0   */
#define AB_B0_PR_1		0x2030	/* -> 6   */
#define AB_B0_PR_2		0x2034	/* -> 0   */

/* ------------------------------------------------------------------ *
 * BAR1 — command/doorbell/status surface (FINDINGS §21, UC_METHODS §13)
 * ------------------------------------------------------------------ */

/* Init writer @0x5a3c BAR1 words */
#define AB_B1_INIT_LO		0x90	/* -> 0xFFFFFE */
#define AB_B1_INIT_HI		0x1090	/* -> 0xFFFFFE */
#define AB_B1_ENGINE_CTRL	0x2010	/* -> 3 (engine descriptor count) */
#define AB_B1_ENGINE_ENABLE	0x2004	/* -> 0x16; written exactly once in the kext, never cleared */

/* Doorbells (fn@0x6800): clear-before-ring. Send channel is live-dead for
 * chains (modes 10/11) — production default is batch/receive. */
#define AB_B1_DOORBELL_SEND	0x4	/* ring value 0xFFE7F   */
#define AB_B1_DOORBELL_BATCH	0x1004	/* ring value 0x80FFE7F */

/* Staging banks: desc-PA lo/hi + local_val (3 words each) */
#define AB_B1_STAGE_SEND	0x4080
#define AB_B1_STAGE_BATCH	0x5080

/* Completion — ENGINE-OWNED. Ack write-backs to +0x2018/+0x2014 are ignored by
 * the engine (readback != written value); never try to clear them (§3n). */
#define AB_B1_COMPLETION	0x2044	/* bit0 valid, [31:1] count */
#define AB_B1_ACK_0		0x2018
#define AB_B1_ACK_1		0x2014

/* Data words (R-to-C pair) */
#define AB_B1_DATA_SEND		0x44
#define AB_B1_DATA_BATCH	0x1044

#endif /* _AB_REGS_H_ */
