/* SPDX-License-Identifier: GPL-2.0 */
/*
 * decoder.h — background decode thread feeding a small frame queue.
 *
 * The card is depth-1 with a single output region, so the thread decodes
 * one frame at a time and copies each result out of the UC-mapped region
 * into one of DECODER_SLOTS cached buffers. The UI thread takes frames by
 * presentation sequence number; ProRes is intra-only, so a seek is simply
 * "decode frame N next" (generation bump + queue flush).
 */
#ifndef AB_DECODER_H
#define AB_DECODER_H

#include <stdbool.h>
#include <stdint.h>

#include "card.h"
#include "mov.h"

#define DECODER_SLOTS 4 /* 1 on screen + 3 decoded ahead */

typedef struct Decoder Decoder;

typedef struct {
	uint64_t index;   /* frame number in the file */
	uint64_t seq;     /* presentation sequence, strictly increasing, >= 1 */
	uint32_t gen;     /* seek generation this frame belongs to */
	uint8_t *data;    /* stride*height bytes, card layout [U,Y0,V,Y1] */
	CardTimings t;
	int slot;
} Frame;

typedef enum { DECODER_EV_ERROR, DECODER_EV_EOS } DecoderEventType;

typedef struct {
	DecoderEventType type;
	uint64_t frame;
	char msg[512];
} DecoderEvent;

typedef struct {
	uint64_t decoded;
	uint64_t recycled;          /* free-run: queued frames overwritten unseen */
	double   decode_fps;        /* completions/s over the last ~1 s window */
	CardTimings last;
	uint64_t sum_decode_ns, max_decode_ns, sum_copy_ns, sum_read_ns;
	uint64_t hist[6];           /* decode: <2, 2-5, 5-10, 10-20, 20-50, >=50 ms */
} DecoderStats;

/* Called from the decode thread after a frame is queued or an event posted. */
typedef void (*DecoderWakeFn)(void *ud);

Decoder *decoder_new(Card *card, const MovInfo *mi, int fd, DecoderWakeFn wake, void *ud);
void decoder_free(Decoder *d); /* joins the thread */

void decoder_set_playing(Decoder *d, bool playing);
void decoder_set_loop(Decoder *d, bool loop);
void decoder_set_free_run(Decoder *d, bool free_run);
void decoder_seek(Decoder *d, uint64_t index); /* flush + decode index next */

/* Take the newest queued frame with seq <= max_seq (older ones are released
 * and counted in *dropped) — or, if the head frame belongs to a generation
 * other than gen (a seek result), that head frame immediately. NULL if none.
 * The caller owns the frame until decoder_release(). */
Frame *decoder_take(Decoder *d, uint32_t gen, uint64_t max_seq, unsigned *dropped);
void decoder_release(Decoder *d, Frame *f);

bool decoder_drained(Decoder *d);   /* hit EOF while playing, queue empty, idle */
bool decoder_pop_event(Decoder *d, DecoderEvent *ev);
void decoder_stats(Decoder *d, DecoderStats *s);
void decoder_reset_stats(Decoder *d);

#endif
