/* abplayer.c — realtime ProRes player through the Afterburner card.
 *
 * Flow: select file -> verify (MOV/MP4 walk + codec/res/fps check) -> Go:
 * continuous async frame submits to /dev/afterburner0, decoded YUYV frames
 * straight into an SDL2 window (card output is interleaved [U,Y,V,Y] =
 * SDL_PIXELFORMAT_YUY2, so the texture update is a zero-conversion blit).
 *
 * Verified pipeline facts baked in here:
 *  - in_len must be the EXACT packet size (page-aligned stalls firmware)
 *  - WAIT_COMPL returns on the batch-channel event (status count>=1);
 *    the delayed send-channel event is drained by the driver pre-ring
 *  - out_len = stride*rows with stride=ceil(W*2/64)*64, rows=ceil(H/16)*16;
 *    only the first H rows are real (padding rows repeat the last row)
 *  - a wedged engine recovers via AB_IOC_START (decoder disable/enable)
 *
 * Usage: abplayer [file.mov] [--start N] [--count N] [-s] [--headless]
 *                [--no-pace] [--dump-dir DIR]
 *   no file arg -> interactive picker over .mov/.mp4 files in cwd
 *   -s          sync submit path (SUBMIT_FRAME, 30 s poll) instead of async
 *   --headless  decode without SDL window (still paces to source framerate)
 *   --no-pace   run as fast as the card allows (verification runs)
 *   --dump-dir DIR  write each decoded frame's raw card output (stride*rows,
 *                   interleaved [U,Y0,V,Y1]) to DIR/frame_<absidx>.yuv;
 *                   implies --headless
 * Keys: q/ESC quit, space pause/resume, f fullscreen toggle.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/types.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>

/* ---- ioctl ABI (must match ab_chardev.c / abctl.c) ---------------------- */
struct ioc_submit { __u32 in_off, in_len, out_len, poll_ms, status, data; };
struct ioc_query  { __u32 status, data_word; };
struct ioc_bind   { __u32 in_size, out_size; };
struct ioc_status { __u32 fw_version, ready, dec_enable, engine_ctrl; };
struct ioc_async  { __u32 in_off, in_len, out_len; };

#define IOC_SUBMIT_FRAME _IOWR('A', 0, struct ioc_submit)
#define IOC_QUERY_COMPL  _IOR ('A', 1, struct ioc_query)
#define IOC_BIND_FRAME   _IOWR('A', 2, struct ioc_bind)
#define IOC_CARD_STATUS  _IOR ('A', 3, struct ioc_status)
#define IOC_UNBIND_ALL   _IO  ('A', 5)
#define IOC_START        _IO  ('A', 8)
#define IOC_SUBMIT_ASYNC _IOWR('A', 9, struct ioc_async)
#define IOC_WAIT_COMPL   _IOR ('A',10, struct ioc_query)
#define IOC_STOP         _IO  ('A', 7)                       /* stop (kext fn@0xc0a4) */

static const char *DEV = "/dev/afterburner0";

/* ---- run statistics (P-B3) ------------------------------------------------- */
struct run_stats {
	uint64_t submitted, completed, failed;
	uint64_t late;             /* frame elapsed > 1.5x source budget        */
	uint64_t wait_retries;     /* second WAIT_COMPL window consumed         */
	uint64_t recovers_full;    /* full unbind/START/rebind ladder (async)   */
	uint64_t recovers_start;   /* START-only recovery (sync path)           */
	uint64_t dt_min, dt_max, dt_sum;  /* per-frame decode time, ns          */
	uint64_t hist[6];          /* <2ms, 2-5, 5-10, 10-20, 20-50, >=50 ms   */
};
static struct run_stats rs;

static void stat_decode_ns(uint64_t ns)
{
	if (!rs.completed || ns < rs.dt_min) rs.dt_min = ns;
	if (ns > rs.dt_max) rs.dt_max = ns;
	rs.dt_sum += ns;
	uint64_t ms = ns / 1000000;
	int b = ms < 2 ? 0 : ms < 5 ? 1 : ms < 10 ? 2 : ms < 20 ? 3 : ms < 50 ? 4 : 5;
	rs.hist[b]++;
}

static void stat_print(void)
{
	printf("== stats: %" PRIu64 " frames (%" PRIu64 " ok / %" PRIu64 " failed), late=%" PRIu64
	       ", recoveries full=%" PRIu64 " start-only=%" PRIu64 ", wait-retries=%" PRIu64 "\n",
	       rs.submitted, rs.completed, rs.failed, rs.late,
	       rs.recovers_full, rs.recovers_start, rs.wait_retries);
	if (rs.completed) {
		printf("   decode time: min=%.2f ms avg=%.2f ms max=%.2f ms | "
		       "<2ms:%" PRIu64 " 2-5:%" PRIu64 " 5-10:%" PRIu64
		       " 10-20:%" PRIu64 " 20-50:%" PRIu64 " >=50:%" PRIu64 "\n",
		       rs.dt_min / 1e6, (double)rs.dt_sum / rs.completed / 1e6,
		       rs.dt_max / 1e6, rs.hist[0], rs.hist[1], rs.hist[2],
		       rs.hist[3], rs.hist[4], rs.hist[5]);
	}
}

/* ---- MOV/MP4 ProRes track parsing (port of tmp/extract_frames.py) ------- */
struct frame_table {
	uint8_t fourcc[4];
	char    profile_name[32];
	int     w, h;
	uint64_t n;
	uint32_t *sizes;      /* per-sample byte counts            */
	uint64_t *offsets;    /* per-sample file offsets           */
	double  fps;
};

static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

/* walk sibling boxes in [start,end); returns count, fills cb */
typedef int (*box_cb)(const char *type, uint64_t s, uint64_t e, void *ud);
static size_t box_walk(const uint8_t *d, uint64_t start, uint64_t end,
		       box_cb cb, void *ud)
{
	size_t found = 0;
	uint64_t off = start;
	while (off + 8 <= end) {
		uint32_t size = be32(d + off);
		const char *type = (const char *)(d + off + 4);
		if (size == 1)      size = (uint32_t)be64(d + off + 8);
		else if (size == 0) size = (uint32_t)(end - off);
		if (size < 8 || off + (uint64_t)size > end) break;
		if (cb(type, off, off + size, ud)) { found++; break; }
		off += size;
	}
	return found;
}

struct find_res { const char *type; uint64_t s, e; int hit; };
static int find_cb(const char *t, uint64_t s, uint64_t e, void *ud)
{
	struct find_res *r = ud;
	if (!r->hit && !memcmp(t, r->type, 4)) { r->s = s; r->e = e; r->hit = 1; }
	return r->hit;
}
static int find_child(const uint8_t *d, uint64_t s, uint64_t e, const char *t,
		      uint64_t *cs, uint64_t *ce)
{
	struct find_res r = { .type = t };
	box_walk(d, s + 8, e, find_cb, &r);
	if (!r.hit) return -1;
	*cs = r.s; *ce = r.e;
	return 0;
}

/* parse one trak; returns 0 and fills ft if it is a video track */
static int parse_track(const uint8_t *d, uint64_t ts, uint64_t te, struct frame_table *ft)
{
	uint64_t ms, me, hs, he, ns, ne, bs, be;
	if (find_child(d, ts, te, "mdia", &ms, &me)) return -1;

	hs = 0;
	if (!find_child(d, ms, me, "hdlr", &hs, &he) && memcmp(d + hs + 16, "vide", 4))
		return -1; /* audio or other track */
	if (find_child(d, ms, me, "minf", &ns, &ne)) return -1;
	if (find_child(d, ns, ne, "stbl", &bs, &be)) return -1;

	uint64_t ss_s, ss_e;
	if (find_child(d, bs, be, "stsd", &ss_s, &ss_e)) return -1;
	/* FullBox: v/flags @+8, entry_count @+12, first entry @+16 */
	uint32_t n_entries = be32(d + ss_s + 12);
	if (n_entries < 1) return -1;
	uint64_t es = ss_s + 16;
	memcpy(ft->fourcc, d + es + 4, 4);
	ft->w = be32(d + es + 32) >> 16 & 0xFFFF;
	ft->h = be32(d + es + 34) >> 16 & 0xFFFF;
	if (!ft->w || !ft->h) return -1;

	/* stsz: spec [ss@+8][count@+12] vs FullBox-style [v/f@+8][ss@+12][count@+16] */
	uint64_t sz_s, sz_e;
	if (find_child(d, bs, be, "stsz", &sz_s, &sz_e)) return -1;
	{
		uint32_t a = be32(d + sz_s + 8);
		uint32_t box_sz = (uint32_t)(sz_e - sz_s);
		uint32_t sample_size, n, p_rel;
		if (a == 0 && box_sz == 16 + 4 * be32(d + sz_s + 12)) {
			sample_size = 0; n = be32(d + sz_s + 12); p_rel = 16;   /* spec var */
		} else if (box_sz == 16) {
			sample_size = a; n = be32(d + sz_s + 12); p_rel = 0;    /* spec fixed */
		} else {
			uint32_t vflags = a, ssz = be32(d + sz_s + 12);
			sample_size = ssz; n = be32(d + sz_s + 16); p_rel = 20; /* FullBox var */
			(void)vflags;
			if (sample_size) { p_rel = 0; }
			else if (box_sz != 20 + 4 * n) return -1;
		}
		ft->n = n;
		ft->sizes = calloc(n ? n : 1, sizeof(uint32_t));
		if (!ft->sizes) return -1;
		for (uint64_t i = 0; i < n; i++)
			ft->sizes[i] = sample_size ? sample_size : be32(d + sz_s + p_rel + 4 * i);
	}

	/* stsc: [count@+8][entries@+12] vs FullBox-style [v/f@+8][count@+12][e@+16] */
	uint64_t sc_s, sc_e;
	if (find_child(d, bs, be, "stsc", &sc_s, &sc_e)) return -1;
	{
		uint32_t n_ent, p_rel;
		if (((sc_e - sc_s) - 12) % 12 == 0) { n_ent = be32(d + sc_s + 8);  p_rel = 12; }
		else                                { n_ent = be32(d + sc_s + 12); p_rel = 16; }
		uint32_t *fc = malloc(n_ent * sizeof(uint32_t));
		uint32_t *spc = malloc(n_ent * sizeof(uint32_t));
		if (!fc || !spc) return -1;
		for (uint32_t i = 0; i < n_ent; i++) {
			fc[i]  = be32(d + sc_s + p_rel + 12 * i);
			spc[i] = be32(d + sc_s + p_rel + 12 * i + 4);
		}

		/* stco/co64: same dual layout; entry size disambiguates */
		uint64_t co_s, co_e;
		int is_co64 = !find_child(d, bs, be, "co64", &co_s, &co_e);
		if (!is_co64 && find_child(d, bs, be, "stco", &co_s, &co_e)) return -1;
		uint32_t esz = is_co64 ? 8 : 4;
		uint32_t box_sz = (uint32_t)(co_e - co_s);
		uint32_t n_ch_a = be32(d + co_s + 8), n_ch, cp_rel;
		if (8 + esz * n_ch_a == box_sz) { n_ch = n_ch_a; cp_rel = 12; }
		else                            { n_ch = be32(d + co_s + 12); cp_rel = 16;
						  if (16 + esz * n_ch != box_sz) return -1; }

		ft->offsets = calloc(ft->n ? ft->n : 1, sizeof(uint64_t));
		if (!ft->offsets) return -1;
		uint32_t si = 0;
		for (uint32_t ei = 0; ei < n_ent && si < ft->n; ei++) {
			/* stsc entry i covers chunks fc[i]..fc[i+1]-1 (1-based); the
			 * last entry runs to chunk n_ch. Off-by-one here double-counts
			 * the boundary chunk and corrupts every sample offset after it
			 * (observed: samples 728-780 pointed at a large-frame base, so
			 * the card received a truncated frame fragment and wedged). */
			uint32_t last_fc = (ei + 1 < n_ent) ? fc[ei + 1] - 1 : n_ch;
			for (uint32_t ci = fc[ei] - 1; ci < last_fc && si < ft->n; ci++) {
				uint64_t base = is_co64 ? be64(d + co_s + cp_rel + esz * ci)
							: be32(d + co_s + cp_rel + esz * ci);
				uint64_t acc = 0;
				for (uint32_t k = 0; k < spc[ei] && si < ft->n; k++) {
					ft->offsets[si] = base + acc;
					acc += ft->sizes[si];
					si++;
				}
			}
		}
		free(fc); free(spc);
		if (si != ft->n) return -1; /* sample table inconsistent */
	}

	/* mdhd: v0 timescale@+20 dur@+24 ; v1 timescale@+28 dur@+32(u64) */
	uint64_t mh_s, mh_e;
	if (find_child(d, ms, me, "mdhd", &mh_s, &mh_e)) return -1;
	{
		double ts_, dur_;
		if (d[mh_s + 8] == 1) {
			ts_ = be32(d + mh_s + 28);
			dur_ = (double)be64(d + mh_s + 32);
		} else {
			ts_ = be32(d + mh_s + 20);
			dur_ = (double)be32(d + mh_s + 24);
		}
		ft->fps = (ts_ > 0 && dur_ > 0 && ft->n) ? ts_ / (dur_ / ft->n) : 0.0;
	}

	static const struct { const char *fcc, *name; } profs[] = {
		{ "apco", "ProRes 422 Proxy" }, { "apcn", "ProRes 422 Standard" },
		{ "apch", "ProRes 422 HQ" },    { "ap4x", "ProRes 422 LT" },
	};
	for (size_t i = 0; i < sizeof(profs)/sizeof(*profs); i++)
		if (!memcmp(ft->fourcc, profs[i].fcc, 4))
			snprintf(ft->profile_name, sizeof(ft->profile_name), "%s", profs[i].name);

	return ft->profile_name[0] ? 0 : -2; /* unknown fourcc */
}

static int verify_file(const char *path, struct frame_table *ft)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) { perror(path); return -1; }
	struct stat st;
	if (fstat(fd, &st) || st.st_size < 64) { close(fd); fprintf(stderr, "not a MOV/MP4\n"); return -1; }
	uint8_t *d = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (d == MAP_FAILED) { perror("mmap file"); return -1; }

	int rc = -1;
	struct find_res moov = { .type = "moov" };
	box_walk(d, 0, st.st_size, find_cb, &moov);
	if (!moov.hit) fprintf(stderr, "REJECT: no moov box (not a MOV/MP4)\n");
	else {
		uint64_t ts, te;
		while (!find_child(d, moov.s, moov.e, "trak", &ts, &te)) {
			int r = parse_track(d, ts, te, ft);
			if (r == 0) { rc = 0; break; }
			if (r == -2) { /* unknown codec: report and stop */
				fprintf(stderr, "REJECT: unsupported codec '%.4s' "
					"(card decodes apco/apcn/apch/ap4x only)\n", ft->fourcc);
				break;
			}
			memset(ft, 0, sizeof(*ft)); /* try next trak */
		}
		if (rc) fprintf(stderr, "REJECT: no card-decodable video track\n");
	}
	munmap(d, st.st_size);
	return rc;
}

/* ---- card ---------------------------------------------------------------- */
struct card {
	int fd;
	uint8_t *inmap, *outmap;
	size_t in_size, out_len;
};

static int card_open(struct card *c, size_t in_size, size_t out_len)
{
	c->fd = open(DEV, O_RDWR);
	if (c->fd < 0) { perror(DEV); return -1; }

	struct ioc_status st;
	if (ioctl(c->fd, IOC_CARD_STATUS, &st)) { perror("CARD_STATUS"); goto err; }
	printf("[card] fw=%u.%u ready=%u dec_enable=0x%x engine_ctrl=0x%x\n",
	       (st.fw_version >> 16) & 0xFFFF, st.fw_version & 0xFFFF, st.ready, st.dec_enable,
	       st.engine_ctrl);
	if (!st.ready) {
		/* Self-contained lifecycle (kext parity): bring the decoder up on first
		 * use instead of requiring a manual `abctl start` — our own STOP-on-exit
		 * (P-B2) leaves ready=0, and so does any prior client's teardown. START
		 * is the same rung-A cycle the recovery ladder uses; proven safe. */
		printf("[card] not ready — auto-starting (AB_IOC_START)\n");
		if (ioctl(c->fd, IOC_START)) { perror("START"); goto err; }
		if (ioctl(c->fd, IOC_CARD_STATUS, &st)) { perror("CARD_STATUS"); goto err; }
	}
	if (!st.ready) { fprintf(stderr, "card not ready after auto-start — check dmesg\n"); goto err; }

	struct ioc_bind b = { (uint32_t)in_size, (uint32_t)out_len };
	if (ioctl(c->fd, IOC_BIND_FRAME, &b)) { perror("BIND_FRAME"); goto err; }

	c->inmap  = mmap(NULL, in_size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
	c->outmap = mmap(NULL, out_len, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 4096);
	if (c->inmap == MAP_FAILED || c->outmap == MAP_FAILED) { perror("mmap"); goto err; }
	c->in_size = in_size; c->out_len = out_len;
	return 0;
err:
	close(c->fd);
	return -1;
}

static void card_close(struct card *c)
{
	if (c->fd >= 0) { ioctl(c->fd, IOC_UNBIND_ALL); close(c->fd); c->fd = -1; }
}

/* Full wedge recovery (driver-documented escalation): UNBIND_ALL clears the
 * stuck in_flight counter and drains pending events; AB_IOC_START then cycles
 * the decoder (disable/enable + init writer + ready poll); re-bind + re-mmap,
 * since unbind freed the DMA regions. The SDL texture keeps working — main()
 * always blits from c->outmap, which now points at the fresh region. */
static int card_recover(struct card *c)
{
	munmap(c->inmap, c->in_size);
	munmap(c->outmap, c->out_len);

	if (ioctl(c->fd, IOC_UNBIND_ALL)) { perror("UNBIND_ALL"); return -1; }
	if (ioctl(c->fd, IOC_START))      { perror("START");    return -1; }

	struct ioc_bind b = { (uint32_t)c->in_size, (uint32_t)c->out_len };
	if (ioctl(c->fd, IOC_BIND_FRAME, &b)) { perror("BIND_FRAME"); return -1; }

	c->inmap  = mmap(NULL, c->in_size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
	c->outmap = mmap(NULL, c->out_len, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 4096);
	if (c->inmap == MAP_FAILED || c->outmap == MAP_FAILED) { perror("mmap"); return -1; }
	return 0;
}

/* Async submit with the driver's documented recovery ladder:
 *   ring -> WAIT_COMPL(2s) -> retry WAIT_COMPL (timeout leaves the frame
 *   pending, kext queryCompletionGated semantics) -> full recover + resubmit.
 * The packet is re-staged into c->inmap before every ring — a recovered bind
 * hands out fresh DMA regions that must be refilled. */
static int card_submit_async(struct card *c, const uint8_t *pkt, size_t pkt_len)
{
	struct ioc_query q;
	int rc;

	memcpy(c->inmap, pkt, pkt_len); /* exact packet bytes — never padded */
	{
		struct ioc_async a = { 0, (uint32_t)pkt_len, (uint32_t)c->out_len };
		if (ioctl(c->fd, IOC_SUBMIT_ASYNC, &a)) return -1;
	}

	rc = ioctl(c->fd, IOC_WAIT_COMPL, &q);
	if (!rc && (q.status >> 1) >= 1) return 0; /* batch channel: frame done */

	rs.wait_retries++;
	fprintf(stderr, "[warn] no completion in window — retrying WAIT_COMPL\n");
	rc = ioctl(c->fd, IOC_WAIT_COMPL, &q);
	if (!rc && (q.status >> 1) >= 1) return 0;

	rs.recovers_full++;
	fprintf(stderr, "[warn] engine wedged — full recovery (unbind/START/rebind)\n");
	if (card_recover(c)) return -1;
	memcpy(c->inmap, pkt, pkt_len); /* fresh input region after rebind */
	{
		struct ioc_async a = { 0, (uint32_t)pkt_len, (uint32_t)c->out_len };
		if (ioctl(c->fd, IOC_SUBMIT_ASYNC, &a)) return -1;
	}
	/* The resubmitted frame can complete slowly too — live run 2026-11-14:
	 * a wedged frame's completion arrived ~4-6 s after the ring (posted as
	 * status=0x2, valid bit clear), and the immediate resubmit also needed
	 * >2 s. Give it three 2 s windows before declaring failure. */
	for (int w = 0; w < 3; w++) {
		rc = ioctl(c->fd, IOC_WAIT_COMPL, &q);
		if (!rc && (q.status >> 1) >= 1) return 0;
	}
	return -1;
}

/* Sync submit: SUBMIT_FRAME polls internally and self-resets on timeout
 * (in_flight cleared), so the only escalation needed is a decoder cycle. */
static int card_submit_sync(struct card *c, const uint8_t *pkt, size_t pkt_len)
{
	struct ioc_submit s;

	memcpy(c->inmap, pkt, pkt_len);
	memset(&s, 0, sizeof(s));
	s.in_len = (uint32_t)pkt_len;
	s.out_len = (uint32_t)c->out_len;
	s.poll_ms = 5000;
	if (!ioctl(c->fd, IOC_SUBMIT_FRAME, &s) && ((s.status >> 1) >= 1))
		return 0;

	rs.recovers_start++;
	fprintf(stderr, "[warn] sync submit failed — recovery: AB_IOC_START + resubmit\n");
	if (ioctl(c->fd, IOC_START)) { perror("START"); return -1; }
	memset(&s, 0, sizeof(s));
	s.in_len = (uint32_t)pkt_len;
	s.out_len = (uint32_t)c->out_len;
	s.poll_ms = 5000;
	if (!ioctl(c->fd, IOC_SUBMIT_FRAME, &s) && ((s.status >> 1) >= 1)) {
		printf("[warn] recovered — frame submitted after restart\n");
		return 0;
	}
	fprintf(stderr, "[error] submit still failing after recovery (%s)\n", strerror(errno));
	return -1;
}

static int card_submit(struct card *c, const uint8_t *pkt, size_t pkt_len, int sync)
{
	return sync ? card_submit_sync(c, pkt, pkt_len)
		    : card_submit_async(c, pkt, pkt_len);
}

/* ---- timing --------------------------------------------------------------- */
static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ---- file picker ---------------------------------------------------------- */
static const char *pick_file(void)
{
	static char path[4096];
	struct dirent **list = NULL;
	int n = scandir(".", &list, NULL, alphasort);
	if (n < 0) return NULL;
	const char *cands[256]; int nc = 0;
	for (int i = 0; i < n && nc < 256; i++) {
		const char *nm = list[i]->d_name;
		size_t l = strlen(nm);
		if ((l > 4 && !strcasecmp(nm + l - 4, ".mov")) ||
		    (l > 4 && !strcasecmp(nm + l - 4, ".mp4")))
			cands[nc++] = nm;
		free(list[i]);
	}
	free(list);
	if (!nc) { fprintf(stderr, "no .mov/.mp4 files in cwd\n"); return NULL; }
	printf("Available video files:\n");
	for (int i = 0; i < nc; i++) printf("  [%d] %s\n", i + 1, cands[i]);
	int ch;
	printf("Select [1-%d]: ", nc);
	if (scanf("%d", &ch) != 1 || ch < 1 || ch > nc) return NULL;
	snprintf(path, sizeof(path), "%s", cands[ch - 1]);
	return path;
}

/* ---- main ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
	const char *path = NULL;
	uint64_t start_frame = 0, count = UINT64_MAX;
	int sync_mode = 0, headless = 0, no_pace = 0;
	const char *dump_dir = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s")) sync_mode = 1;
		else if (!strcmp(argv[i], "--headless")) headless = 1;
		else if (!strcmp(argv[i], "--no-pace")) no_pace = 1;
		else if (!strcmp(argv[i], "--dump-dir") && i + 1 < argc) dump_dir = argv[++i];
		else if (!strcmp(argv[i], "--start") && i + 1 < argc) start_frame = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = strtoull(argv[++i], NULL, 0);
		else if (path == NULL) path = argv[i];
	}
	if (dump_dir) headless = 1; /* dump mode is a verification run */
	if (!path) path = pick_file();
	if (!path) return 1;

	printf("== verify: %s\n", path);
	struct frame_table ft;
	memset(&ft, 0, sizeof(ft));
	if (verify_file(path, &ft)) return 2;

	size_t stride = ((size_t)ft.w * 2 + 63) / 64 * 64;
	size_t rows   = ((size_t)ft.h + 15) / 16 * 16;
	size_t out_len = stride * rows;
	uint32_t max_sample = 0;
	for (uint64_t i = 0; i < ft.n; i++) if (ft.sizes[i] > max_sample) max_sample = ft.sizes[i];
	size_t in_size = (max_sample + 4095) & ~(size_t)4095;

	printf("OK: %s (%.4s) %dx%d, %" PRIu64 " frames @ %.3f fps\n",
	       ft.profile_name, ft.fourcc, ft.w, ft.h, ft.n, ft.fps);
	printf("    in=%zu B (max sample %u), out/frame=%zu B (stride %zu x rows %zu)\n",
	       in_size, max_sample, out_len, stride, rows);

	uint64_t last = start_frame + count;
	if (start_frame >= ft.n) { fprintf(stderr, "--start beyond EOF\n"); return 2; }
	if (last > ft.n) last = ft.n;
	printf("== go: frames %" PRIu64 " .. %" PRIu64 " (%" PRIu64 ")\n",
	       start_frame, last - 1, last - start_frame);

	/* re-mmap the file read-only for zero-copy packet reads */
	int ffd = open(path, O_RDONLY);
	if (ffd < 0) { perror(path); return 1; }
	struct stat fst;
	fstat(ffd, &fst);
	const uint8_t *fdata = mmap(NULL, fst.st_size, PROT_READ, MAP_PRIVATE, ffd, 0);
	close(ffd);
	if (fdata == MAP_FAILED) { perror("mmap file"); return 1; }

	/* P-B1: startup offset self-check — validate EVERY sample before touching
	 * hardware. The stsc off-by-one class (frames 728-780, ce48cf4) corrupts
	 * offsets silently mid-stream; a ProRes packet's own declared length is the
	 * u32BE at offset 0, so be32(offset)==size must hold for all samples. Any
	 * mismatch means we'd feed a truncated/shifted fragment to the engine and
	 * wedge it — fail loudly at launch instead (bounds check included). */
	{
		uint64_t bad = 0;
		for (uint64_t i = 0; i < ft.n; i++) {
			if (ft.offsets[i] + ft.sizes[i] > (uint64_t)fst.st_size ||
			    be32(fdata + ft.offsets[i]) != ft.sizes[i]) {
				if (bad < 10)
					fprintf(stderr, "offset self-check: sample %" PRIu64
						" bad (off=%" PRIu64 " size=%u)\n",
						i, ft.offsets[i], ft.sizes[i]);
				bad++;
			}
		}
		if (bad) {
			fprintf(stderr, "== ABORT: %" PRIu64
				" sample(s) failed offset self-check — not touching hardware\n", bad);
			munmap((void *)fdata, fst.st_size);
			return 2;
		}
		printf("offset self-check: all %" PRIu64 " samples OK (be32(off)==size + bounds)\n", ft.n);
	}

	struct card c = { .fd = -1 };
	if (card_open(&c, in_size, out_len)) return 1;

	/* SDL viewer: YUY2 texture pointed at the card's output DMA region */
	SDL_Window *win = NULL;
	SDL_Renderer *ren = NULL;
	SDL_Texture *tex = NULL;
	if (!headless) {
		if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
		win = SDL_CreateWindow("Afterburner Player",
					   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
					   ft.w, ft.h, SDL_WINDOW_SHOWN);
		if (!win) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
		if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
		if (!ren) { fprintf(stderr, "SDL renderer: %s\n", SDL_GetError()); return 1; }
		tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_YUY2,
				     SDL_TEXTUREACCESS_STREAMING, ft.w, ft.h);
		if (!tex) { fprintf(stderr, "SDL texture: %s\n", SDL_GetError()); return 1; }
	}

	uint64_t interval_ns = ft.fps > 0 ? (uint64_t)(1e9 / ft.fps) : 0;
	int running = 1, paused = 0, fullscreen = 0;
	double fps_ema = 0.0;
	uint64_t t_prev = now_ns();

	uint64_t failed_frame = UINT64_MAX;

	for (uint64_t i = start_frame; i < last && running; i++) {
			if (!headless) {
				SDL_Event ev;
				while (SDL_PollEvent(&ev)) {
					if (ev.type == SDL_QUIT) running = 0;
					else if (ev.type == SDL_KEYDOWN) {
						switch (ev.key.keysym.sym) {
						case SDLK_q: case SDLK_ESCAPE: running = 0; break;
						case SDLK_SPACE: paused = !paused; break;
						case SDLK_f: fullscreen ^= 1;
							SDL_SetWindowFullscreen(win, fullscreen); break;
						}
					}
				}
			}
			if (paused) { SDL_Delay(50); continue; }

			uint64_t t0 = now_ns();
			const uint8_t *pkt = fdata + ft.offsets[i];
			size_t plen = ft.sizes[i];

			rs.submitted++;
			if (card_submit(&c, pkt, plen, sync_mode)) { rs.failed++; failed_frame = i; running = 0; break; }
			uint64_t t_dec = now_ns() - t0;
			rs.completed++; stat_decode_ns(t_dec);

			if (dump_dir) { /* capture raw card output before next submit */
				char dpath[4096];
				snprintf(dpath, sizeof(dpath), "%s/frame_%" PRIu64 ".yuv", dump_dir, i);
				int dd = open(dpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
				if (dd < 0) { perror(dpath); rs.failed++; failed_frame = i; running = 0; break; }
				ssize_t wr = write(dd, c.outmap, out_len);
				close(dd);
				if (wr != (ssize_t)out_len) {
					fprintf(stderr, "dump frame %" PRIu64 ": short write %zd\n", i, wr);
					rs.failed++; failed_frame = i; running = 0; break;
				}
			}

			if (!headless) {
				SDL_Rect dst = { 0, 0, ft.w, ft.h };
				SDL_UpdateTexture(tex, &dst, c.outmap, (int)stride);
				SDL_RenderClear(ren);
				SDL_RenderCopy(ren, tex, NULL, NULL);
				SDL_RenderPresent(ren);
			}

			uint64_t t_frame = now_ns() - t_prev;
			t_prev = now_ns();
			fps_ema = fps_ema ? fps_ema * 0.9 + (1e9 / (double)t_frame) * 0.1
					  : 1e9 / (double)t_frame;

			if (!headless) {
				char title[256];
				snprintf(title, sizeof(title),
					 "Afterburner — %s %.4s | frame %" PRIu64 "/%" PRIu64
					 " | %.1f fps | decode %.0f ms%s",
					 ft.profile_name, ft.fourcc, i - start_frame + 1,
					 last - start_frame, fps_ema, t_dec / 1e6,
					 paused ? " | PAUSED" : "");
				SDL_SetWindowTitle(win, title);
			} else if (dump_dir || i % 30 == 0) {
				printf("[frame %" PRIu64 "] decode %.0f ms\n", i, t_dec / 1e6);
			}

			if (interval_ns && !no_pace) { /* pace to source framerate */
				uint64_t elapsed = now_ns() - t0;
				if (elapsed < interval_ns) {
					struct timespec rq = {
						.tv_sec  = (interval_ns - elapsed) / 1000000000ull,
						.tv_nsec = (long)((interval_ns - elapsed) % 1000000000ull),
					};
					while (nanosleep(&rq, &rq) == -1 && errno == EINTR) {
						SDL_PumpEvents(); /* stay responsive */
					}
				} else if (elapsed > interval_ns * 3 / 2) {
					rs.late++;
					fprintf(stderr, "[warn] frame %" PRIu64 " late: %llu ms "
						"(decode %llu ms) budget %llu ms\n", i,
						(unsigned long long)(elapsed / 1000000),
						(unsigned long long)(t_dec / 1000000),
						(unsigned long long)(interval_ns / 1000000));
				}
			}
		}

	stat_print();
	if (failed_frame != UINT64_MAX) {
		fprintf(stderr, "== ABORTED at frame %" PRIu64 " — submit failed after full recovery\n",
			failed_frame);
	} else {
		printf("== done\n");
	}

	/* P-B2: stop-cmd on exit — kext parity (§40.5): quiesce the decoder so the
	 * card idles with it disabled instead of left enabled. EBUSY = a frame is
	 * still in flight (aborted mid-submit); UNBIND_ALL below drains that. */
	if (c.fd >= 0 && ioctl(c.fd, IOC_STOP) == -1 && errno != EBUSY)
		fprintf(stderr, "stop: %s\n", strerror(errno));

	munmap((void *)fdata, fst.st_size);
	card_close(&c);
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	free(ft.sizes); free(ft.offsets);
	return failed_frame != UINT64_MAX ? 1 : 0;
}
