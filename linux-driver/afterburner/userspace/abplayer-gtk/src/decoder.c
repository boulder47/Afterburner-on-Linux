// SPDX-License-Identifier: GPL-2.0
/*
 * decoder.c — background decode thread + frame queue (see decoder.h).
 */
#define _GNU_SOURCE
#include "decoder.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SLOT_FREE, SLOT_DECODING, SLOT_QUEUED, SLOT_HELD };

#define EV_MAX 8

struct Decoder {
	Card *card;
	const MovInfo *mi;
	int fd;
	DecoderWakeFn wake;
	void *ud;

	pthread_t thread;
	pthread_mutex_t m;
	pthread_cond_t cv;

	Frame slots[DECODER_SLOTS];
	int state[DECODER_SLOTS];
	int queue[DECODER_SLOTS]; /* FIFO of slot ids */
	int qhead, qlen;

	bool playing, loop, free_run, quit;
	bool one_shot;  /* decode exactly one frame (seek while paused) */
	bool at_end;    /* reached EOF while playing (no loop) */
	bool busy;      /* a card_decode() is in progress */
	bool failed;    /* stop decoding until the user seeks/plays again */
	uint64_t next;
	uint32_t gen;
	uint64_t seq;

	DecoderEvent ev[EV_MAX];
	int ev_head, ev_len;

	DecoderStats st;
	uint64_t win_t0, win_n;
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* caller holds d->m */
static void post_event(Decoder *d, DecoderEventType type, uint64_t frame, const char *msg)
{
	DecoderEvent *e;

	if (d->ev_len == EV_MAX) { /* drop the oldest */
		d->ev_head = (d->ev_head + 1) % EV_MAX;
		d->ev_len--;
	}
	e = &d->ev[(d->ev_head + d->ev_len) % EV_MAX];
	e->type = type;
	e->frame = frame;
	snprintf(e->msg, sizeof(e->msg), "%s", msg ? msg : "");
	d->ev_len++;
}

static int q_pop(Decoder *d)
{
	int s = d->queue[d->qhead];

	d->qhead = (d->qhead + 1) % DECODER_SLOTS;
	d->qlen--;
	return s;
}

static void q_push(Decoder *d, int s)
{
	d->queue[(d->qhead + d->qlen) % DECODER_SLOTS] = s;
	d->qlen++;
}

static void q_flush(Decoder *d)
{
	while (d->qlen)
		d->state[q_pop(d)] = SLOT_FREE;
}

static void account(Decoder *d, const CardTimings *t)
{
	uint64_t ms = t->decode_ns / 1000000, now = now_ns();
	int b = ms < 2 ? 0 : ms < 5 ? 1 : ms < 10 ? 2 : ms < 20 ? 3 : ms < 50 ? 4 : 5;

	d->st.decoded++;
	d->st.last = *t;
	d->st.sum_decode_ns += t->decode_ns;
	d->st.sum_copy_ns += t->copy_ns;
	d->st.sum_read_ns += t->read_ns;
	if (t->decode_ns > d->st.max_decode_ns)
		d->st.max_decode_ns = t->decode_ns;
	d->st.hist[b]++;

	d->win_n++;
	if (!d->win_t0)
		d->win_t0 = now;
	if (now - d->win_t0 >= 1000000000ull) {
		d->st.decode_fps = (double)d->win_n * 1e9 / (double)(now - d->win_t0);
		d->win_n = 0;
		d->win_t0 = now;
	}
}

static void *decode_thread(void *arg)
{
	Decoder *d = arg;

	pthread_mutex_lock(&d->m);
	while (!d->quit) {
		bool want = (d->playing || d->one_shot) && !d->failed;
		int s = -1;
		uint64_t idx;
		uint32_t gen;
		CardTimings t;
		char err[512];
		int rc;

		if (want && d->next >= d->mi->n_frames) {
			if (d->loop && d->playing && !d->one_shot) {
				d->next = 0; /* seamless: same generation, seq keeps counting */
			} else {
				if (d->playing && !d->at_end) {
					d->at_end = true;
					post_event(d, DECODER_EV_EOS, d->mi->n_frames - 1, NULL);
					pthread_mutex_unlock(&d->m);
					d->wake(d->ud);
					pthread_mutex_lock(&d->m);
				}
				d->one_shot = false;
				want = false;
			}
		}
		if (want) {
			for (int i = 0; i < DECODER_SLOTS; i++)
				if (d->state[i] == SLOT_FREE) {
					s = i;
					break;
				}
			if (s < 0 && d->free_run && d->qlen) {
				s = q_pop(d); /* benchmark: never stall on the display */
				d->st.recycled++;
			}
		}
		if (s < 0) {
			pthread_cond_wait(&d->cv, &d->m);
			continue;
		}

		d->state[s] = SLOT_DECODING;
		d->busy = true;
		idx = d->next;
		gen = d->gen;
		pthread_mutex_unlock(&d->m);

		rc = card_decode(d->card, d->fd, d->mi->offsets[idx], d->mi->sizes[idx], idx,
				 d->slots[s].data, &t, err, sizeof(err));

		pthread_mutex_lock(&d->m);
		d->busy = false;
		if (rc) {
			d->state[s] = SLOT_FREE;
			d->failed = true;
			d->one_shot = false;
			post_event(d, DECODER_EV_ERROR, idx, err);
			pthread_mutex_unlock(&d->m);
			d->wake(d->ud);
			pthread_mutex_lock(&d->m);
			continue;
		}
		account(d, &t);
		if (gen != d->gen) { /* seek landed meanwhile: stale */
			d->state[s] = SLOT_FREE;
			continue;
		}
		d->slots[s].index = idx;
		d->slots[s].seq = ++d->seq;
		d->slots[s].gen = gen;
		d->slots[s].t = t;
		d->state[s] = SLOT_QUEUED;
		q_push(d, s);
		d->next = idx + 1;
		d->one_shot = false;
		d->at_end = false;

		pthread_mutex_unlock(&d->m);
		d->wake(d->ud);
		pthread_mutex_lock(&d->m);
	}
	pthread_mutex_unlock(&d->m);
	return NULL;
}

Decoder *decoder_new(Card *card, const MovInfo *mi, int fd, DecoderWakeFn wake, void *ud)
{
	Decoder *d = calloc(1, sizeof(*d));
	size_t bytes = (size_t)mi->out_stride * mi->height;

	if (!d)
		return NULL;
	d->card = card;
	d->mi = mi;
	d->fd = fd;
	d->wake = wake;
	d->ud = ud;
	for (int i = 0; i < DECODER_SLOTS; i++) {
		d->slots[i].slot = i;
		d->slots[i].data = malloc(bytes);
		if (!d->slots[i].data) {
			for (int k = 0; k < i; k++)
				free(d->slots[k].data);
			free(d);
			return NULL;
		}
	}
	pthread_mutex_init(&d->m, NULL);
	pthread_cond_init(&d->cv, NULL);
	if (pthread_create(&d->thread, NULL, decode_thread, d)) {
		for (int i = 0; i < DECODER_SLOTS; i++)
			free(d->slots[i].data);
		free(d);
		return NULL;
	}
	pthread_setname_np(d->thread, "ab-decode");
	return d;
}

void decoder_free(Decoder *d)
{
	if (!d)
		return;
	pthread_mutex_lock(&d->m);
	d->quit = true;
	pthread_cond_broadcast(&d->cv);
	pthread_mutex_unlock(&d->m);
	pthread_join(d->thread, NULL);
	for (int i = 0; i < DECODER_SLOTS; i++)
		free(d->slots[i].data);
	pthread_cond_destroy(&d->cv);
	pthread_mutex_destroy(&d->m);
	free(d);
}

void decoder_set_playing(Decoder *d, bool playing)
{
	pthread_mutex_lock(&d->m);
	d->playing = playing;
	if (playing)
		d->failed = false;
	pthread_cond_broadcast(&d->cv);
	pthread_mutex_unlock(&d->m);
}

void decoder_set_loop(Decoder *d, bool loop)
{
	pthread_mutex_lock(&d->m);
	d->loop = loop;
	if (loop)
		d->at_end = false;
	pthread_cond_broadcast(&d->cv);
	pthread_mutex_unlock(&d->m);
}

void decoder_set_free_run(Decoder *d, bool free_run)
{
	pthread_mutex_lock(&d->m);
	d->free_run = free_run;
	pthread_cond_broadcast(&d->cv);
	pthread_mutex_unlock(&d->m);
}

void decoder_seek(Decoder *d, uint64_t index)
{
	pthread_mutex_lock(&d->m);
	if (index >= d->mi->n_frames)
		index = d->mi->n_frames - 1;
	d->gen++;
	q_flush(d);
	d->next = index;
	d->one_shot = true;
	d->at_end = false;
	d->failed = false;
	pthread_cond_broadcast(&d->cv);
	pthread_mutex_unlock(&d->m);
}

Frame *decoder_take(Decoder *d, uint32_t gen, uint64_t max_seq, unsigned *dropped)
{
	Frame *out = NULL;
	unsigned drop = 0;

	pthread_mutex_lock(&d->m);
	if (d->qlen && d->slots[d->queue[d->qhead]].gen != gen) {
		int s = q_pop(d);

		d->state[s] = SLOT_HELD;
		out = &d->slots[s];
	} else {
		while (d->qlen && d->slots[d->queue[d->qhead]].seq <= max_seq) {
			int s = q_pop(d);

			if (out) {
				d->state[out->slot] = SLOT_FREE;
				drop++;
			}
			d->state[s] = SLOT_HELD;
			out = &d->slots[s];
		}
	}
	if (drop)
		pthread_cond_broadcast(&d->cv);
	pthread_mutex_unlock(&d->m);
	if (dropped)
		*dropped = drop;
	return out;
}

void decoder_release(Decoder *d, Frame *f)
{
	if (!f)
		return;
	pthread_mutex_lock(&d->m);
	d->state[f->slot] = SLOT_FREE;
	pthread_cond_broadcast(&d->cv);
	pthread_mutex_unlock(&d->m);
}

bool decoder_drained(Decoder *d)
{
	bool r;

	pthread_mutex_lock(&d->m);
	r = d->at_end && !d->qlen && !d->busy;
	pthread_mutex_unlock(&d->m);
	return r;
}

bool decoder_pop_event(Decoder *d, DecoderEvent *ev)
{
	bool r = false;

	pthread_mutex_lock(&d->m);
	if (d->ev_len) {
		*ev = d->ev[d->ev_head];
		d->ev_head = (d->ev_head + 1) % EV_MAX;
		d->ev_len--;
		r = true;
	}
	pthread_mutex_unlock(&d->m);
	return r;
}

void decoder_stats(Decoder *d, DecoderStats *s)
{
	pthread_mutex_lock(&d->m);
	*s = d->st;
	if (d->win_t0 && now_ns() - d->win_t0 > 2000000000ull)
		s->decode_fps = 0; /* idle */
	pthread_mutex_unlock(&d->m);
}

void decoder_reset_stats(Decoder *d)
{
	pthread_mutex_lock(&d->m);
	memset(&d->st, 0, sizeof(d->st));
	d->win_t0 = 0;
	d->win_n = 0;
	pthread_mutex_unlock(&d->m);
}
