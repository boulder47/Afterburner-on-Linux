/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mov.h — MOV/MP4 ProRes video-track parser + per-frame self-check.
 *
 * Bounds-checked rewrite of abplayer's parser for a GUI that accepts any
 * dropped file: strict ISO-BMFF/QuickTime FullBox layouts, 64-bit box sizes
 * (largesize), no reads outside the box being parsed.
 */
#ifndef AB_MOV_H
#define AB_MOV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	PRORES_SUPPORTED = 0,  /* pixel-verified on fw 2.62 (apco/apcn/apch) */
	PRORES_UNTESTED,       /* 422 family but never run on the card (apcs) */
	PRORES_UNSUPPORTED,    /* engine acks but emits nothing (4444, RAW)  */
} ProresSupport;

typedef struct {
	uint64_t file_size;

	char     fourcc[5];
	const char *profile;          /* "ProRes 422 HQ" etc. */
	ProresSupport support;

	/* geometry — width/height come from the ProRes frame header, which is
	 * what the firmware sizes its output from (FINDINGS §35) */
	uint32_t width, height;
	uint32_t stsd_width, stsd_height;
	uint32_t par_num, par_den;    /* pixel aspect from 'pasp' (1:1 default) */

	/* sample table */
	uint64_t  n_frames;
	uint32_t *sizes;
	uint64_t *offsets;
	uint32_t  max_sample;

	/* timing */
	uint32_t timescale;
	uint64_t duration_ts;         /* sum of stts deltas */
	uint32_t frame_delta;         /* constant delta; 0 if variable */
	double   fps;

	/* colour: 'colr' box if present, else the frame header */
	bool     has_colr;
	uint16_t primaries, transfer, matrix;
	bool     full_range;

	/* first frame header */
	uint8_t  chroma;              /* 2 = 4:2:2, 3 = 4:4:4 */
	uint8_t  interlace;           /* 0 progressive, 1 TFF, 2 BFF */
	uint8_t  hdr_primaries, hdr_transfer, hdr_matrix;

	/* card buffer geometry (FINDINGS §35.3) */
	uint32_t out_stride;          /* ceil(W*2/64)*64 */
	uint32_t out_rows;            /* ceil(H/16)*16   */
	uint32_t out_len;             /* stride*rows     */
	uint32_t in_size;             /* max sample, page aligned */
} MovInfo;

/* Parse container + sample tables. 0 on success, else -1 with err filled. */
int mov_parse(int fd, MovInfo *mi, char *err, size_t errlen);

/* Validate every sample before the card ever sees it (abplayer P-B1, extended):
 * in-file bounds, be32(off)==size, 'icpf' tag, constant W/H, 4:2:2 chroma.
 * progress(done,total) is called periodically; *cancel aborts. Fills the
 * frame-header fields of mi from sample 0. 0 = all good, -1 = reject. */
typedef void (*MovProgressFn)(uint64_t done, uint64_t total, void *ud);
int mov_selfcheck(int fd, MovInfo *mi, MovProgressFn progress, void *ud,
		  const volatile int *cancel, char *err, size_t errlen);

void mov_free(MovInfo *mi);

/* Effective Y'CbCr matrix for display: 1 = BT.709, 6 = BT.601, 9 = BT.2020 */
int mov_effective_matrix(const MovInfo *mi);
const char *mov_matrix_name(int m);
const char *mov_primaries_name(int p);
const char *mov_transfer_name(int t);

#endif
