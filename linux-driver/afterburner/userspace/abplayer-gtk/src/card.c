// SPDX-License-Identifier: GPL-2.0
/*
 * card.c — /dev/afterburner0 backend + simulator.
 *
 * The hardware path is a port of userspace/abplayer.c's proven sequence
 * (FINDINGS §41–43): BIND_FRAME both regions, mmap input (pgoff 0) and output
 * (pgoff 1), stage the EXACT packet bytes, SUBMIT_ASYNC, WAIT_COMPL until the
 * batch-channel completion (status>>1 >= 1), with the documented ladder:
 * second WAIT_COMPL window -> full unbind/START/rebind + resubmit (3 windows).
 *
 * One rule this file is careful about: BIND_FRAME, UNBIND_ALL and RESET free
 * the coherent DMA pages in the driver, while remap_pfn_range() mappings hold
 * no page references — so every mapping is torn down BEFORE any of those
 * ioctls, never after (a stale mapping would alias freed kernel memory).
 */
#define _GNU_SOURCE
#include "card.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- ioctl ABI (must match ab_chardev.c) ---------------------------------- */
struct ab_ioc_submit { __u32 in_off, in_len, out_len, poll_ms, status, data_word; };
struct ab_ioc_query  { __u32 status, data_word; };
struct ab_ioc_bind   { __u32 in_size, out_size; };
struct ab_ioc_status { __u32 fw_version, ready, dec_enable, engine_ctrl; };
struct ab_ioc_async  { __u32 in_off, in_len, out_len; };

#define AB_IOC_BIND_FRAME   _IOWR('A', 2, struct ab_ioc_bind)
#define AB_IOC_CARD_STATUS  _IOR ('A', 3, struct ab_ioc_status)
#define AB_IOC_UNBIND_ALL   _IO  ('A', 5)
#define AB_IOC_GET_PERCENT  _IOR ('A', 6, __u32)
#define AB_IOC_STOP         _IO  ('A', 7)
#define AB_IOC_START        _IO  ('A', 8)
#define AB_IOC_SUBMIT_ASYNC _IOWR('A', 9, struct ab_ioc_async)
#define AB_IOC_WAIT_COMPL   _IOR ('A', 10, struct ab_ioc_query)
#define AB_IOC_RESET        _IO  ('A', 11)

struct Card {
	bool sim;
	int fd;
	pthread_mutex_t lock;   /* serialises decode vs control commands */
	pthread_mutex_t slock;  /* counters only — never held across ioctls */
	CardCounters cnt;

	CardGeometry g;
	bool bound;
	bool stopped;           /* decoder disabled: START before next decode */
	size_t page;
	uint8_t *inmap, *outmap;
	size_t in_map_len, out_map_len;
	uint8_t *stage;         /* cached staging buffer for the packet read */

	/* simulator */
	uint8_t *sim_out, *sim_tmpl;
	int sim_replay_fd;
	uint64_t sim_replay_frames;
	uint64_t sim_busy_ns, sim_pct_t0;
};

static void seterr(char *err, size_t n, const char *fmt, ...)
{
	va_list ap;

	if (!err || !n)
		return;
	va_start(ap, fmt);
	vsnprintf(err, n, fmt, ap);
	va_end(ap);
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r == -1 && errno == EINTR);
	return r;
}

static int pread_full(int fd, void *buf, size_t len, uint64_t off)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t r = pread(fd, p, len, (off_t)off);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			if (r == 0)
				errno = EIO;
			return -1;
		}
		p += r;
		len -= (size_t)r;
		off += (uint64_t)r;
	}
	return 0;
}

static void count(Card *c, uint64_t *field)
{
	pthread_mutex_lock(&c->slock);
	(*field)++;
	pthread_mutex_unlock(&c->slock);
}

static Card *card_alloc(void)
{
	Card *c = calloc(1, sizeof(*c));

	if (!c)
		return NULL;
	c->fd = -1;
	c->sim_replay_fd = -1;
	c->page = (size_t)sysconf(_SC_PAGESIZE);
	pthread_mutex_init(&c->lock, NULL);
	pthread_mutex_init(&c->slock, NULL);
	return c;
}

bool card_is_sim(const Card *c) { return c->sim; }

const char *card_name(const Card *c)
{
	return c->sim ? "Simulated card (no hardware)" : AB_DEVICE_PATH;
}

/* ======================================================================== *
 * hardware
 * ======================================================================== */

static void hw_unmap(Card *c)
{
	if (c->inmap && c->inmap != MAP_FAILED)
		munmap(c->inmap, c->in_map_len);
	if (c->outmap && c->outmap != MAP_FAILED)
		munmap(c->outmap, c->out_map_len);
	c->inmap = c->outmap = NULL;
}

/* caller holds c->lock; mappings must already be gone */
static int hw_bind_map(Card *c, char *err, size_t errlen)
{
	struct ab_ioc_bind b = { c->g.in_size, c->g.out_len };

	c->in_map_len = ((size_t)c->g.in_size + c->page - 1) / c->page * c->page;
	c->out_map_len = ((size_t)c->g.out_len + c->page - 1) / c->page * c->page;

	if (xioctl(c->fd, AB_IOC_BIND_FRAME, &b)) {
		seterr(err, errlen, "BIND_FRAME (%u + %u bytes) failed: %s", b.in_size,
		       b.out_size, strerror(errno));
		c->bound = false;
		return -1;
	}
	c->inmap = mmap(NULL, c->in_map_len, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
	c->outmap = mmap(NULL, c->out_map_len, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd,
			 (off_t)c->page);
	if (c->inmap == MAP_FAILED || c->outmap == MAP_FAILED) {
		int e = errno;

		hw_unmap(c);
		xioctl(c->fd, AB_IOC_UNBIND_ALL, NULL);
		seterr(err, errlen, "mmap of DMA regions failed: %s", strerror(e));
		c->bound = false;
		return -1;
	}
	c->bound = true;
	return 0;
}

static int hw_status(Card *c, CardStatus *st)
{
	struct ab_ioc_status s;

	if (xioctl(c->fd, AB_IOC_CARD_STATUS, &s))
		return -1;
	st->fw_version = s.fw_version;
	st->ready = s.ready;
	st->dec_enable = s.dec_enable;
	st->engine_ctrl = s.engine_ctrl;
	return 0;
}

Card *card_open_hw(const char *dev, char *err, size_t errlen)
{
	Card *c = card_alloc();
	CardStatus st;

	if (!c) {
		seterr(err, errlen, "out of memory");
		return NULL;
	}
	c->fd = open(dev, O_RDWR | O_CLOEXEC);
	if (c->fd < 0) {
		int e = errno;

		if (e == ENOENT)
			seterr(err, errlen, "%s does not exist — is the afterburner driver loaded? "
			       "(cd linux-driver/afterburner && make && sudo insmod afterburner.ko)", dev);
		else if (e == EACCES)
			seterr(err, errlen, "permission denied opening %s", dev);
		else
			seterr(err, errlen, "cannot open %s: %s", dev, strerror(e));
		goto fail;
	}
	/* the driver keeps ONE global binding: a second client's BIND frees the
	 * first one's regions under it. abplayer and abctl's mutating commands
	 * take the same lock. */
	if (flock(c->fd, LOCK_EX | LOCK_NB)) {
		seterr(err, errlen, errno == EWOULDBLOCK
		       ? "the card is in use by another client (abplayer, abctl or another "
			 "Afterburner Player window) — close it and try again"
		       : "flock on %s failed: %s", dev, strerror(errno));
		goto fail;
	}
	if (hw_status(c, &st)) {
		seterr(err, errlen, "CARD_STATUS failed: %s", strerror(errno));
		goto fail;
	}
	if (!st.ready) {
		/* abplayer P-B2: a previous client's STOP-on-exit leaves ready=0 */
		if (xioctl(c->fd, AB_IOC_START, NULL) || hw_status(c, &st) || !st.ready) {
			seterr(err, errlen, "decoder not ready and AB_IOC_START did not bring it up (%s) — "
			       "check dmesg", errno ? strerror(errno) : "ready=0");
			goto fail;
		}
		c->cnt.auto_starts++;
	}
	return c;
fail:
	if (c->fd >= 0)
		close(c->fd);
	pthread_mutex_destroy(&c->lock);
	pthread_mutex_destroy(&c->slock);
	free(c);
	return NULL;
}

/* full wedge recovery — abplayer card_recover(): unmap FIRST, then
 * UNBIND_ALL (clears the stuck in_flight + drains events), START (decoder
 * disable/enable cycle), rebind + remap. Caller holds c->lock. */
static int hw_recover(Card *c, char *err, size_t errlen)
{
	hw_unmap(c);
	if (xioctl(c->fd, AB_IOC_UNBIND_ALL, NULL)) {
		seterr(err, errlen, "recovery: UNBIND_ALL failed: %s", strerror(errno));
		c->bound = false;
		return -1;
	}
	c->bound = false;
	if (xioctl(c->fd, AB_IOC_START, NULL)) {
		seterr(err, errlen, "recovery: START failed: %s", strerror(errno));
		return -1;
	}
	return hw_bind_map(c, err, errlen);
}

static int hw_ring(Card *c, uint32_t len)
{
	struct ab_ioc_async a = { 0, len, c->g.out_len };

	return xioctl(c->fd, AB_IOC_SUBMIT_ASYNC, &a);
}

static bool hw_wait(Card *c)
{
	struct ab_ioc_query q = { 0, 0 };

	return xioctl(c->fd, AB_IOC_WAIT_COMPL, &q) == 0 && (q.status >> 1) >= 1;
}

static int hw_decode_locked(Card *c, uint32_t len, char *err, size_t errlen)
{
	if (c->stopped) {
		if (xioctl(c->fd, AB_IOC_START, NULL)) {
			seterr(err, errlen, "decoder was stopped and START failed: %s", strerror(errno));
			return -1;
		}
		c->stopped = false;
		count(c, &c->cnt.auto_starts);
	}

	memcpy(c->inmap, c->stage, len); /* exact packet bytes — never padded */
	if (hw_ring(c, len) == 0) {
		if (hw_wait(c))
			return 0;
		count(c, &c->cnt.wait_retries);
		if (hw_wait(c)) /* timeout leaves the frame pending (kext semantics) */
			return 0;
	} else if (errno != EBUSY && errno != ENODEV) {
		seterr(err, errlen, "SUBMIT_ASYNC failed: %s", strerror(errno));
		return -1;
	}

	/* wedged (or a stale in-flight frame / decoder down): full ladder */
	count(c, &c->cnt.recovers);
	if (hw_recover(c, err, errlen))
		return -1;
	memcpy(c->inmap, c->stage, len); /* fresh input region after rebind */
	if (hw_ring(c, len)) {
		seterr(err, errlen, "SUBMIT_ASYNC after recovery failed: %s", strerror(errno));
		return -1;
	}
	/* a wedged frame's completion has been seen to arrive 4–6 s late
	 * (abplayer, 2026-11-14): three 2 s windows before giving up */
	for (int w = 0; w < 3; w++)
		if (hw_wait(c))
			return 0;
	seterr(err, errlen, "no completion even after full recovery (unbind/START/rebind) — "
	       "engine wedged; try Card ▸ Stop Decoder, then Start Decoder and reopen the "
	       "clip, and check dmesg");
	return -1;
}

/* ======================================================================== *
 * simulator
 * ======================================================================== */

typedef struct { uint8_t y, u, v; } Yuv;

static Yuv rgb_to_yuv(double r, double g, double b, int matrix)
{
	double kr = 0.2126, kb = 0.0722, y, cb, cr;
	Yuv o;

	if (matrix == 6) { kr = 0.299; kb = 0.114; }
	else if (matrix == 9) { kr = 0.2627; kb = 0.0593; }
	y = kr * r + (1 - kr - kb) * g + kb * b;
	cb = (b - y) / (2 * (1 - kb));
	cr = (r - y) / (2 * (1 - kr));
	o.y = (uint8_t)lround(16 + 219 * y);
	o.u = (uint8_t)lround(128 + 224 * cb);
	o.v = (uint8_t)lround(128 + 224 * cr);
	return o;
}

static void put_px(uint8_t *row, uint32_t x, Yuv c)
{
	uint8_t *g = row + (x / 2) * 4; /* [U, Y0, V, Y1] — card layout */

	g[0] = c.u;
	g[2] = c.v;
	g[(x & 1) ? 3 : 1] = c.y;
}

static void fill_rect(Card *c, uint8_t *buf, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, Yuv col)
{
	if (x1 > c->g.width) x1 = c->g.width;
	if (y1 > c->g.height) y1 = c->g.height;
	for (uint32_t y = y0; y < y1; y++)
		for (uint32_t x = x0; x < x1; x++)
			put_px(buf + (size_t)y * c->g.stride, x, col);
}

static void sim_build_template(Card *c)
{
	static const double bars[7][3] = {
		{ .75, .75, .75 }, { .75, .75, 0 }, { 0, .75, .75 }, { 0, .75, 0 },
		{ .75, 0, .75 }, { .75, 0, 0 }, { 0, 0, .75 },
	};
	uint32_t w = c->g.width, h = c->g.height;
	uint32_t y_bars = h * 58 / 100, y_ramp = h * 67 / 100;

	memset(c->sim_tmpl, 0, (size_t)c->g.stride * c->g.rows);
	for (int i = 0; i < 7; i++)
		fill_rect(c, c->sim_tmpl, w * i / 7, 0, w * (i + 1) / 7, y_bars,
			  rgb_to_yuv(bars[i][0], bars[i][1], bars[i][2], c->g.matrix));
	for (uint32_t y = y_bars; y < y_ramp; y++)
		for (uint32_t x = 0; x < w; x++) {
			double v = w > 1 ? (double)x / (w - 1) : 0;

			put_px(c->sim_tmpl + (size_t)y * c->g.stride, x, rgb_to_yuv(v, v, v, c->g.matrix));
		}
	fill_rect(c, c->sim_tmpl, 0, y_ramp, w, h, rgb_to_yuv(.1, .1, .1, c->g.matrix));
}

static void sim_pad_rows(Card *c, uint8_t *buf)
{
	/* card behaviour: rows H..R-1 repeat the last real row (§35.3) */
	for (uint32_t y = c->g.height; y < c->g.rows; y++)
		memcpy(buf + (size_t)y * c->g.stride, buf + (size_t)(c->g.height - 1) * c->g.stride,
		       c->g.stride);
}

static int sim_paint(Card *c, uint64_t idx, char *err, size_t errlen)
{
	uint32_t w = c->g.width, h = c->g.height;

	if (c->sim_replay_fd >= 0) {
		size_t row = ((size_t)w + 1) / 2 * 4;
		uint64_t fi = idx % c->sim_replay_frames;

		for (uint32_t y = 0; y < h; y++)
			if (pread_full(c->sim_replay_fd, c->sim_out + (size_t)y * c->g.stride, row,
				       (fi * h + y) * row)) {
				seterr(err, errlen, "simulator: replay read failed: %s", strerror(errno));
				return -1;
			}
		sim_pad_rows(c, c->sim_out);
		return 0;
	}

	memcpy(c->sim_out, c->sim_tmpl, (size_t)c->g.stride * c->g.rows);
	{
		/* moving box: one sweep of the frame width every 4 s @30 fps */
		uint32_t y0 = h * 70 / 100, bs = h * 14 / 100 ? h * 14 / 100 : 1;
		uint32_t span = w > bs ? w - bs : 1;
		uint32_t x0 = (uint32_t)((idx * 8ull * w / 960) % span);

		fill_rect(c, c->sim_out, x0, y0, x0 + bs, y0 + bs, rgb_to_yuv(.9, .9, .9, c->g.matrix));
	}
	{
		/* frame index as 16 binary cells (MSB left): verifies stepping */
		uint32_t cw = w / 40 ? w / 40 : 1, y0 = h * 88 / 100, y1 = h * 96 / 100;
		Yuv on = rgb_to_yuv(1, 1, 1, c->g.matrix), off = rgb_to_yuv(0, 0, 0, c->g.matrix);

		for (int b = 0; b < 16; b++) {
			uint32_t x0 = cw + (uint32_t)b * cw * 3 / 2;

			fill_rect(c, c->sim_out, x0, y0, x0 + cw, y1, (idx >> (15 - b)) & 1 ? on : off);
		}
	}
	sim_pad_rows(c, c->sim_out);
	return 0;
}

Card *card_open_sim(char *err, size_t errlen)
{
	Card *c = card_alloc();

	if (!c) {
		seterr(err, errlen, "out of memory");
		return NULL;
	}
	c->sim = true;
	c->sim_pct_t0 = now_ns();
	return c;
}

static int sim_bind(Card *c, char *err, size_t errlen)
{
	const char *replay = getenv("AB_SIM_UYVY");
	size_t out = (size_t)c->g.stride * c->g.rows;

	free(c->sim_out);
	free(c->sim_tmpl);
	c->sim_out = malloc(out);
	c->sim_tmpl = malloc(out);
	if (!c->sim_out || !c->sim_tmpl) {
		seterr(err, errlen, "simulator: out of memory");
		return -1;
	}
	if (c->sim_replay_fd >= 0)
		close(c->sim_replay_fd);
	c->sim_replay_fd = -1;
	if (replay && *replay) {
		struct stat st;
		size_t fsz = ((size_t)c->g.width + 1) / 2 * 4 * c->g.height;

		c->sim_replay_fd = open(replay, O_RDONLY | O_CLOEXEC);
		if (c->sim_replay_fd < 0 || fstat(c->sim_replay_fd, &st) || (size_t)st.st_size < fsz) {
			seterr(err, errlen, "simulator: AB_SIM_UYVY=%s unusable (need ≥ one %ux%u "
			       "uyvy422 frame)", replay, c->g.width, c->g.height);
			if (c->sim_replay_fd >= 0)
				close(c->sim_replay_fd);
			c->sim_replay_fd = -1;
			return -1;
		}
		c->sim_replay_frames = (uint64_t)st.st_size / fsz;
	}
	sim_build_template(c);
	c->bound = true;
	return 0;
}

/* ======================================================================== *
 * common API
 * ======================================================================== */

int card_bind(Card *c, const CardGeometry *g, char *err, size_t errlen)
{
	int rc;
	uint8_t *stage;

	if (!g->width || !g->height || !g->stride || g->rows < g->height || !g->in_size ||
	    (uint64_t)g->stride * g->rows != g->out_len) {
		seterr(err, errlen, "invalid geometry");
		return -1;
	}
	pthread_mutex_lock(&c->lock);
	stage = realloc(c->stage, g->in_size);
	if (!stage) {
		pthread_mutex_unlock(&c->lock);
		seterr(err, errlen, "out of memory");
		return -1;
	}
	c->stage = stage;
	c->g = *g;
	if (c->sim) {
		rc = sim_bind(c, err, errlen);
	} else {
		hw_unmap(c); /* before BIND frees the previous regions */
		rc = hw_bind_map(c, err, errlen);
	}
	pthread_mutex_unlock(&c->lock);
	return rc;
}

int card_decode(Card *c, int fd, uint64_t off, uint32_t len, uint64_t frame_idx,
		uint8_t *dst, CardTimings *t, char *err, size_t errlen)
{
	uint64_t t0, t1, t2, t3;
	int rc;

	pthread_mutex_lock(&c->lock);
	if (!c->bound) {
		pthread_mutex_unlock(&c->lock);
		seterr(err, errlen, "card has no DMA binding (bind failed or reset pending)");
		return -1;
	}
	if (len > c->g.in_size || len < 8) {
		pthread_mutex_unlock(&c->lock);
		seterr(err, errlen, "packet of %u bytes does not fit the %u-byte input region",
		       len, c->g.in_size);
		return -1;
	}
	count(c, &c->cnt.submitted);

	t0 = now_ns();
	if (pread_full(fd, c->stage, len, off)) {
		int e = errno;

		pthread_mutex_unlock(&c->lock);
		count(c, &c->cnt.failed);
		seterr(err, errlen, "reading frame %llu from the file failed: %s",
		       (unsigned long long)frame_idx, strerror(e));
		return -1;
	}
	t1 = now_ns();

	if (c->sim) {
		uint64_t ns = (uint64_t)c->g.width * c->g.height / 3; /* ~2.8 ms @ 4K (§43.1) */
		struct timespec rq;

		if (ns < 300000)
			ns = 300000;
		if (c->stopped) {
			c->stopped = false;
			count(c, &c->cnt.auto_starts);
		}
		rc = memcmp(c->stage + 4, "icpf", 4) ? (seterr(err, errlen, "simulator: no icpf tag"), -1)
						     : sim_paint(c, frame_idx, err, errlen);
		{
			uint64_t spent = now_ns() - t1;

			if (spent < ns) {
				rq.tv_sec = (time_t)((ns - spent) / 1000000000ull);
				rq.tv_nsec = (long)((ns - spent) % 1000000000ull);
				while (nanosleep(&rq, &rq) && errno == EINTR)
					;
			}
		}
	} else {
		rc = hw_decode_locked(c, len, err, errlen);
	}
	t2 = now_ns();
	if (c->sim) {
		pthread_mutex_lock(&c->slock);
		c->sim_busy_ns += t2 - t1;
		pthread_mutex_unlock(&c->slock);
	}

	if (rc == 0) {
		/* only the real rows; padding rows repeat row H-1 anyway */
		memcpy(dst, c->sim ? c->sim_out : c->outmap, (size_t)c->g.stride * c->g.height);
	}
	t3 = now_ns();
	pthread_mutex_unlock(&c->lock);

	count(c, rc ? &c->cnt.failed : &c->cnt.completed);
	if (t) {
		t->read_ns = t1 - t0;
		t->decode_ns = t2 - t1;
		t->copy_ns = t3 - t2;
	}
	return rc;
}

int card_status(Card *c, CardStatus *st)
{
	if (c->sim) {
		st->fw_version = 0;
		st->ready = c->stopped ? 0 : 1;
		st->dec_enable = c->stopped ? 0 : 1;
		st->engine_ctrl = 0;
		return 0;
	}
	return hw_status(c, st);
}

int card_get_percent(Card *c, uint32_t *pct)
{
	if (c->sim) {
		uint64_t now = now_ns(), win;

		pthread_mutex_lock(&c->slock);
		win = now - c->sim_pct_t0;
		*pct = win ? (uint32_t)(c->sim_busy_ns * 100 / win) : 0;
		if (*pct > 100)
			*pct = 100;
		c->sim_busy_ns = 0;
		c->sim_pct_t0 = now;
		pthread_mutex_unlock(&c->slock);
		return 0;
	}
	return xioctl(c->fd, AB_IOC_GET_PERCENT, pct);
}

void card_counters(Card *c, CardCounters *out)
{
	pthread_mutex_lock(&c->slock);
	*out = c->cnt;
	pthread_mutex_unlock(&c->slock);
}

int card_start(Card *c, char *err, size_t errlen)
{
	int rc = 0;

	pthread_mutex_lock(&c->lock);
	if (!c->sim && xioctl(c->fd, AB_IOC_START, NULL)) {
		seterr(err, errlen, "START failed: %s", strerror(errno));
		rc = -1;
	} else {
		c->stopped = false;
	}
	pthread_mutex_unlock(&c->lock);
	return rc;
}

int card_stop(Card *c, char *err, size_t errlen)
{
	int rc = 0;

	pthread_mutex_lock(&c->lock);
	if (!c->sim && xioctl(c->fd, AB_IOC_STOP, NULL)) {
		seterr(err, errlen, errno == EBUSY ? "STOP refused: a frame is still in flight"
						   : "STOP failed: %s", strerror(errno));
		rc = -1;
	} else {
		c->stopped = true;
	}
	pthread_mutex_unlock(&c->lock);
	return rc;
}

int card_reset(Card *c, char *err, size_t errlen)
{
	int rc = 0;

	pthread_mutex_lock(&c->lock);
	if (c->sim) {
		usleep(50000);
		c->stopped = false;
		pthread_mutex_unlock(&c->lock);
		return 0;
	}
	hw_unmap(c); /* RESET frees the regions */
	if (xioctl(c->fd, AB_IOC_RESET, NULL)) {
		int e = errno;

		/* the driver checks the FLR capability before releasing anything
		 * (-EOPNOTSUPP on the Afterburner: DEVCAP has no FLR, plan2 B2);
		 * rebind regardless so the session is in a known state */
		seterr(err, errlen, e == EOPNOTSUPP
		       ? "hard reset not supported: this card does not advertise PCIe FLR "
			 "(use Stop + Start Decoder instead)"
		       : "RESET (FLR) failed: %s", strerror(e));
		c->bound = false;
		if (c->g.out_len)
			hw_bind_map(c, NULL, 0);
		rc = -1;
	} else {
		CardStatus st;

		c->bound = false;
		c->stopped = false;
		if (hw_status(c, &st) == 0 && !st.ready && xioctl(c->fd, AB_IOC_START, NULL)) {
			seterr(err, errlen, "decoder not ready after reset: %s", strerror(errno));
			rc = -1;
		} else if (c->g.out_len) {
			rc = hw_bind_map(c, err, errlen);
		}
	}
	pthread_mutex_unlock(&c->lock);
	return rc;
}

void card_close(Card *c)
{
	if (!c)
		return;
	pthread_mutex_lock(&c->lock);
	if (!c->sim && c->fd >= 0) {
		/* kext parity (§40.5): idle with the decoder disabled. EBUSY = a
		 * frame is still pending after an abort; UNBIND_ALL drains it. */
		xioctl(c->fd, AB_IOC_STOP, NULL);
		hw_unmap(c);
		xioctl(c->fd, AB_IOC_UNBIND_ALL, NULL);
		close(c->fd);
	}
	if (c->sim_replay_fd >= 0)
		close(c->sim_replay_fd);
	free(c->sim_out);
	free(c->sim_tmpl);
	free(c->stage);
	pthread_mutex_unlock(&c->lock);
	pthread_mutex_destroy(&c->lock);
	pthread_mutex_destroy(&c->slock);
	free(c);
}

int card_sysfs_read(const char *attr, char *buf, size_t len)
{
	char path[256];
	int fd;
	ssize_t r;

	snprintf(path, sizeof(path), AB_SYSFS_DIR "/%s", attr);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	r = read(fd, buf, len - 1);
	close(fd);
	if (r < 0)
		return -1;
	while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' '))
		r--;
	buf[r] = '\0';
	return 0;
}
