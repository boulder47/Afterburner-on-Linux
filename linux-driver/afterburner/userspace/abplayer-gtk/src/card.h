/* SPDX-License-Identifier: GPL-2.0 */
/*
 * card.h — /dev/afterburner0 backend (plus a clearly-labelled simulator).
 *
 * All entry points are thread-safe: decode and the control commands
 * (start/stop/reset) serialise on an internal mutex, and the UC-mapped DMA
 * regions never escape this module — card_decode() copies the decoded frame
 * into a caller buffer while holding the lock, so a concurrent reset/rebind
 * can never leave anyone holding a stale mapping.
 */
#ifndef AB_CARD_H
#define AB_CARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AB_DEVICE_PATH "/dev/afterburner0"
#define AB_SYSFS_DIR   "/sys/class/afterburner/afterburner0"

typedef struct Card Card;

typedef struct {
	uint32_t fw_version;   /* 16.16: 0x0002003E = 2.62 */
	uint32_t ready;
	uint32_t dec_enable;
	uint32_t engine_ctrl;
} CardStatus;

typedef struct {
	uint32_t width, height;   /* from the ProRes frame header */
	uint32_t stride, rows;    /* card output layout (FINDINGS §35.3) */
	uint32_t in_size;         /* page-aligned max sample */
	uint32_t out_len;         /* stride * rows */
	int      matrix;          /* simulator only: 1/6/9 */
} CardGeometry;

typedef struct {
	uint64_t submitted, completed, failed;
	uint64_t wait_retries;    /* second WAIT_COMPL window used */
	uint64_t recovers;        /* full unbind/START/rebind ladder */
	uint64_t auto_starts;     /* decoder was stopped and had to be started */
} CardCounters;

typedef struct {
	uint64_t read_ns;         /* packet pread + stage into the input region */
	uint64_t decode_ns;       /* ring -> batch completion */
	uint64_t copy_ns;         /* UC output region -> caller buffer */
} CardTimings;

/* Open the real card: O_RDWR, exclusive flock (one player at a time),
 * CARD_STATUS, and AB_IOC_START if the decoder is not ready (abplayer P-B2
 * self-contained lifecycle). Returns NULL with err on failure. */
Card *card_open_hw(const char *dev, char *err, size_t errlen);

/* Simulator: no hardware; paints a test pattern per frame (or replays raw
 * UYVY frames from $AB_SIM_UYVY), with card-like timing and output layout. */
Card *card_open_sim(char *err, size_t errlen);

bool card_is_sim(const Card *c);
const char *card_name(const Card *c);

/* (Re)bind DMA regions for a clip. Releases any previous binding first. */
int card_bind(Card *c, const CardGeometry *g, char *err, size_t errlen);

/* Read the packet at file offset off/len from fd, decode it on the card, and
 * copy the first g->height rows (stride bytes each) into dst. Runs the
 * recovery ladder on a wedged engine. frame_idx is bookkeeping only (the
 * simulator uses it to paint). 0 on success, -1 with err. */
int card_decode(Card *c, int fd, uint64_t off, uint32_t len, uint64_t frame_idx,
		uint8_t *dst, CardTimings *t, char *err, size_t errlen);

int card_status(Card *c, CardStatus *st);            /* lock-free register read */
int card_get_percent(Card *c, uint32_t *pct);        /* duty cycle since last call */
void card_counters(Card *c, CardCounters *out);

/* Control commands (block while a decode is in progress). */
int card_start(Card *c, char *err, size_t errlen);   /* AB_IOC_START */
int card_stop(Card *c, char *err, size_t errlen);    /* AB_IOC_STOP  */
int card_reset(Card *c, char *err, size_t errlen);   /* AB_IOC_RESET (FLR) + rebind */

/* STOP (kext parity: idle with decoder disabled), unmap, UNBIND_ALL, close. */
void card_close(Card *c);

/* Read one sysfs attribute of the card into buf (trimmed). 0 on success. */
int card_sysfs_read(const char *attr, char *buf, size_t len);

#endif
