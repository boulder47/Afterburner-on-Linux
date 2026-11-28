// SPDX-License-Identifier: GPL-2.0
/*
 * mov.c — MOV/MP4 ProRes video-track parser + per-frame self-check.
 *
 * Compared with userspace/abplayer.c's parser (written for known-good test
 * clips), this one is meant for arbitrary files dropped onto a window:
 *  - every read is bounds-checked against its enclosing box;
 *  - 64-bit box sizes throughout (a >4 GiB mdat — ~45 s of 4K HQ — uses a
 *    largesize header);
 *  - FourCC table: apco/apcs/apcn/apch accepted (apcs = 422 LT, untested on
 *    the card); 4444 (ap4h, ap4x = XQ) and RAW rejected with a reason;
 *  - the self-check also validates the 'icpf' tag, constant dimensions and
 *    4:2:2 chroma, and works through pread() so a huge file is never mapped.
 */
#define _GNU_SOURCE
#include "mov.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MOOV_MAX     (512ull << 20) /* refuse absurd moov boxes */
#define SAMPLES_MAX  (1ull << 26)   /* 67M frames — ~26 days at 30 fps */
#define DIM_MAX      16384u
#define FRAME_HDR    28             /* bytes of the ProRes frame header we inspect */

static void seterr(char *err, size_t n, const char *fmt, ...)
{
	va_list ap;

	if (!err || !n)
		return;
	va_start(ap, fmt);
	vsnprintf(err, n, fmt, ap);
	va_end(ap);
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static int pread_full(int fd, void *buf, size_t len, uint64_t off)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t r = pread(fd, p, len, (off_t)off);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		p += r;
		len -= (size_t)r;
		off += (uint64_t)r;
	}
	return 0;
}

/* ---- in-memory box navigation ------------------------------------------- */

typedef struct {
	uint64_t start, end, body; /* body = first byte after the box header */
	char type[5];
} Box;

static bool box_at(const uint8_t *d, uint64_t off, uint64_t end, Box *b)
{
	uint64_t size, hdr = 8;

	if (off > end || end - off < 8)
		return false;
	size = be32(d + off);
	if (size == 1) {
		if (end - off < 16)
			return false;
		size = be64(d + off + 8);
		hdr = 16;
	} else if (size == 0) {
		size = end - off; /* extends to end of the enclosing box */
	}
	if (size < hdr || size > end - off)
		return false;
	b->start = off;
	b->end = off + size;
	b->body = off + hdr;
	memcpy(b->type, d + off + 4, 4);
	b->type[4] = '\0';
	return true;
}

static bool child(const uint8_t *d, uint64_t from, uint64_t to, const char *t, Box *out)
{
	Box b;
	uint64_t off = from;

	while (box_at(d, off, to, &b)) {
		if (!memcmp(b.type, t, 4)) {
			*out = b;
			return true;
		}
		off = b.end;
	}
	return false;
}

static uint64_t body_len(const Box *b) { return b->end - b->body; }

/* ---- codec table ---------------------------------------------------------- */

static const struct {
	const char *fcc, *name;
	ProresSupport support;
} prores_tab[] = {
	{ "apco", "ProRes 422 Proxy", PRORES_SUPPORTED },
	{ "apcs", "ProRes 422 LT",    PRORES_UNTESTED },
	{ "apcn", "ProRes 422",       PRORES_SUPPORTED },
	{ "apch", "ProRes 422 HQ",    PRORES_SUPPORTED },
	{ "ap4h", "ProRes 4444",      PRORES_UNSUPPORTED },
	{ "ap4x", "ProRes 4444 XQ",   PRORES_UNSUPPORTED },
	{ "aprn", "ProRes RAW",       PRORES_UNSUPPORTED },
	{ "aprh", "ProRes RAW HQ",    PRORES_UNSUPPORTED },
};

static void printable_fourcc(const uint8_t *p, char out[5])
{
	for (int i = 0; i < 4; i++)
		out[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '?';
	out[4] = '\0';
}

/* ---- track parsing --------------------------------------------------------- */

enum { TRAK_OK = 0, TRAK_SKIP = 1, TRAK_REJECT = 2, TRAK_BAD = -1 };

static void parse_sample_entry_children(const uint8_t *d, const Box *entry, MovInfo *mi)
{
	Box b;
	uint64_t off = entry->start + 86; /* 8 B box hdr + 78 B VisualSampleEntry */

	while (box_at(d, off, entry->end, &b)) {
		uint64_t n = body_len(&b);

		if (!memcmp(b.type, "colr", 4) && n >= 10) {
			const uint8_t *p = d + b.body;

			if (!memcmp(p, "nclc", 4) || !memcmp(p, "nclx", 4)) {
				mi->has_colr = true;
				mi->primaries = be16(p + 4);
				mi->transfer = be16(p + 6);
				mi->matrix = be16(p + 8);
				if (!memcmp(p, "nclx", 4) && n >= 11)
					mi->full_range = (p[10] & 0x80) != 0;
			}
		} else if (!memcmp(b.type, "pasp", 4) && n >= 8) {
			uint32_t h = be32(d + b.body), v = be32(d + b.body + 4);

			if (h && v) {
				mi->par_num = h;
				mi->par_den = v;
			}
		}
		off = b.end;
	}
}

static int parse_trak(const uint8_t *d, const Box *trak, MovInfo *mi, char *err, size_t errlen)
{
	Box mdia, hdlr, mdhd, minf, stbl, stsd, stts, stsz, stsc, stco, entry;
	bool co64;
	const uint8_t *p;

	if (!child(d, trak->body, trak->end, "mdia", &mdia))
		return TRAK_SKIP;
	if (!child(d, mdia.body, mdia.end, "hdlr", &hdlr) || body_len(&hdlr) < 12 ||
	    memcmp(d + hdlr.body + 8, "vide", 4))
		return TRAK_SKIP; /* audio, timecode, ... */

	if (!child(d, mdia.body, mdia.end, "minf", &minf) ||
	    !child(d, minf.body, minf.end, "stbl", &stbl) ||
	    !child(d, stbl.body, stbl.end, "stsd", &stsd)) {
		seterr(err, errlen, "video track has no sample description (stsd)");
		return TRAK_BAD;
	}

	/* stsd: FullBox v/f(4) entry_count(4) entries... */
	if (body_len(&stsd) < 8 || be32(d + stsd.body + 4) < 1 ||
	    !box_at(d, stsd.body + 8, stsd.end, &entry) || entry.end - entry.start < 86) {
		seterr(err, errlen, "malformed video sample description");
		return TRAK_BAD;
	}
	printable_fourcc(d + entry.start + 4, mi->fourcc);
	mi->profile = NULL;
	for (size_t i = 0; i < sizeof(prores_tab) / sizeof(*prores_tab); i++)
		if (!memcmp(mi->fourcc, prores_tab[i].fcc, 4)) {
			mi->profile = prores_tab[i].name;
			mi->support = prores_tab[i].support;
		}
	if (!mi->profile) {
		seterr(err, errlen, "video codec '%s' is not ProRes — the Afterburner card "
		       "decodes ProRes 422 only", mi->fourcc);
		return TRAK_REJECT;
	}
	if (mi->support == PRORES_UNSUPPORTED) {
		seterr(err, errlen, "%s ('%s') is not decoded by Afterburner firmware 2.62 — "
		       "the engine acknowledges these frames but emits no output "
		       "(FINDINGS §36–37). Use ProRes 422 Proxy/LT/422/HQ.",
		       mi->profile, mi->fourcc);
		return TRAK_REJECT;
	}
	mi->stsd_width = be16(d + entry.start + 32);
	mi->stsd_height = be16(d + entry.start + 34);
	mi->par_num = mi->par_den = 1;
	parse_sample_entry_children(d, &entry, mi);

	/* mdhd: timescale @ v0 body+12, v1 body+20 */
	if (!child(d, mdia.body, mdia.end, "mdhd", &mdhd) || body_len(&mdhd) < 24) {
		seterr(err, errlen, "video track has no media header (mdhd)");
		return TRAK_BAD;
	}
	p = d + mdhd.body;
	if (p[0] == 1) {
		if (body_len(&mdhd) < 32) {
			seterr(err, errlen, "truncated mdhd box");
			return TRAK_BAD;
		}
		mi->timescale = be32(p + 20);
	} else {
		mi->timescale = be32(p + 12);
	}
	if (!mi->timescale) {
		seterr(err, errlen, "media timescale is zero");
		return TRAK_BAD;
	}

	/* stsz: v/f(4) sample_size(4) sample_count(4) [sizes...] */
	if (!child(d, stbl.body, stbl.end, "stsz", &stsz) || body_len(&stsz) < 12) {
		seterr(err, errlen, "missing/short sample size table (stsz%s)",
		       child(d, stbl.body, stbl.end, "stz2", &stsz) ? " — compact stz2 not supported" : "");
		return TRAK_BAD;
	}
	{
		uint32_t fixed = be32(d + stsz.body + 4);
		uint64_t n = be32(d + stsz.body + 8);

		if (!n || n > SAMPLES_MAX) {
			seterr(err, errlen, "video track has %llu samples", (unsigned long long)n);
			return TRAK_BAD;
		}
		if (!fixed && body_len(&stsz) < 12 + 4 * n) {
			seterr(err, errlen, "sample size table truncated (%llu entries declared)",
			       (unsigned long long)n);
			return TRAK_BAD;
		}
		mi->n_frames = n;
		mi->sizes = calloc(n, sizeof(*mi->sizes));
		mi->offsets = calloc(n, sizeof(*mi->offsets));
		if (!mi->sizes || !mi->offsets) {
			seterr(err, errlen, "out of memory for %llu samples", (unsigned long long)n);
			return TRAK_BAD;
		}
		mi->max_sample = 0;
		for (uint64_t i = 0; i < n; i++) {
			mi->sizes[i] = fixed ? fixed : be32(d + stsz.body + 12 + 4 * i);
			if (mi->sizes[i] > mi->max_sample)
				mi->max_sample = mi->sizes[i];
		}
	}

	/* stts: v/f(4) count(4) {sample_count, delta}... */
	if (!child(d, stbl.body, stbl.end, "stts", &stts) || body_len(&stts) < 8) {
		seterr(err, errlen, "missing time-to-sample table (stts)");
		return TRAK_BAD;
	}
	{
		uint32_t ne = be32(d + stts.body + 4);
		uint64_t covered = 0, dur = 0;
		uint32_t first_delta = 0;
		bool constant = true;

		if (body_len(&stts) < 8 + 8ull * ne) {
			seterr(err, errlen, "time-to-sample table truncated");
			return TRAK_BAD;
		}
		for (uint32_t i = 0; i < ne; i++) {
			uint32_t cnt = be32(d + stts.body + 8 + 8ull * i);
			uint32_t delta = be32(d + stts.body + 12 + 8ull * i);

			if (!cnt)
				continue;
			if (!first_delta)
				first_delta = delta;
			/* a different delta on the very last sample is a common muxer
			 * artefact — still a constant-rate clip */
			if (delta != first_delta && !(covered + cnt == mi->n_frames && cnt == 1))
				constant = false;
			covered += cnt;
			dur += (uint64_t)cnt * delta;
		}
		mi->duration_ts = dur;
		mi->frame_delta = (constant && first_delta) ? first_delta : 0;
		if (mi->frame_delta)
			mi->fps = (double)mi->timescale / mi->frame_delta;
		else if (dur)
			mi->fps = (double)mi->timescale * (double)mi->n_frames / (double)dur;
		else
			mi->fps = 0.0;
		if (!(mi->fps > 0.5 && mi->fps < 1000.0)) {
			seterr(err, errlen, "implausible frame rate (%.3f fps)", mi->fps);
			return TRAK_BAD;
		}
		if (!mi->duration_ts)
			mi->duration_ts = (uint64_t)mi->n_frames * mi->frame_delta;
	}

	/* chunk offsets: stco (u32) or co64 (u64); v/f(4) count(4) entries... */
	co64 = child(d, stbl.body, stbl.end, "co64", &stco);
	if (!co64 && !child(d, stbl.body, stbl.end, "stco", &stco)) {
		seterr(err, errlen, "missing chunk offset table (stco/co64)");
		return TRAK_BAD;
	}
	if (!child(d, stbl.body, stbl.end, "stsc", &stsc) || body_len(&stsc) < 8 ||
	    body_len(&stco) < 8) {
		seterr(err, errlen, "missing sample-to-chunk table (stsc)");
		return TRAK_BAD;
	}
	{
		uint32_t esz = co64 ? 8 : 4;
		uint64_t n_ch = be32(d + stco.body + 4);
		uint32_t n_sc = be32(d + stsc.body + 4);
		uint64_t si = 0;
		uint32_t prev_first = 0;

		if (body_len(&stco) < 8 + esz * n_ch) {
			seterr(err, errlen, "chunk offset table truncated");
			return TRAK_BAD;
		}
		if (body_len(&stsc) < 8 + 12ull * n_sc || !n_sc || !n_ch) {
			seterr(err, errlen, "sample-to-chunk table truncated or empty");
			return TRAK_BAD;
		}
		for (uint32_t e = 0; e < n_sc && si < mi->n_frames; e++) {
			const uint8_t *ent = d + stsc.body + 8 + 12ull * e;
			uint32_t first = be32(ent), spc = be32(ent + 4);
			/* entry e covers chunks [first, next_first) — 1-based; the
			 * last entry runs to the final chunk (abplayer ce48cf4 fix) */
			uint64_t next_first = (e + 1 < n_sc) ? be32(ent + 12) : n_ch + 1;

			if (first < 1 || first <= prev_first || first > n_ch ||
			    next_first <= first || next_first > n_ch + 1) {
				seterr(err, errlen, "inconsistent sample-to-chunk entry %u", e);
				return TRAK_BAD;
			}
			prev_first = first;
			for (uint64_t ch = first; ch < next_first && si < mi->n_frames; ch++) {
				const uint8_t *cp = d + stco.body + 8 + esz * (ch - 1);
				uint64_t off = co64 ? be64(cp) : be32(cp);

				for (uint32_t k = 0; k < spc && si < mi->n_frames; k++) {
					mi->offsets[si] = off;
					off += mi->sizes[si];
					si++;
				}
			}
		}
		if (si != mi->n_frames) {
			seterr(err, errlen, "sample table inconsistent: chunks hold %llu of %llu samples",
			       (unsigned long long)si, (unsigned long long)mi->n_frames);
			return TRAK_BAD;
		}
	}
	return TRAK_OK;
}

static void reset_track(MovInfo *mi)
{
	uint64_t fsz = mi->file_size;

	free(mi->sizes);
	free(mi->offsets);
	memset(mi, 0, sizeof(*mi));
	mi->file_size = fsz;
}

int mov_parse(int fd, MovInfo *mi, char *err, size_t errlen)
{
	struct stat st;
	uint64_t off = 0, moov_off = 0, moov_len = 0;
	bool have_moov = false, have_moof = false, truncated = false;
	uint8_t *moov = NULL;
	char first_err[512] = "";
	int rc = -1;
	Box mb, trak;

	memset(mi, 0, sizeof(*mi));
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		seterr(err, errlen, "not a regular file");
		return -1;
	}
	mi->file_size = (uint64_t)st.st_size;

	/* top-level walk via pread — never maps the (possibly huge) mdat */
	while (off + 8 <= mi->file_size) {
		uint8_t h[16];
		uint64_t size, hdr = 8;
		size_t want = mi->file_size - off >= 16 ? 16 : 8;

		if (pread_full(fd, h, want, off)) {
			seterr(err, errlen, "read error: %s", strerror(errno ? errno : EIO));
			return -1;
		}
		size = be32(h);
		if (size == 1) {
			if (want < 16) {
				truncated = true;
				break;
			}
			size = be64(h + 8);
			hdr = 16;
		} else if (size == 0) {
			size = mi->file_size - off;
		}
		if (size < hdr || size > mi->file_size - off) {
			truncated = true;
			break;
		}
		if (!memcmp(h + 4, "moov", 4) && !have_moov) {
			have_moov = true;
			moov_off = off;
			moov_len = size;
		} else if (!memcmp(h + 4, "moof", 4)) {
			have_moof = true;
		}
		off += size;
	}
	if (!have_moov) {
		seterr(err, errlen, truncated ? "no movie header found — file is truncated or not a MOV/MP4"
					      : "no movie header (moov) — not a MOV/MP4 file");
		return -1;
	}
	if (moov_len > MOOV_MAX) {
		seterr(err, errlen, "movie header is implausibly large (%llu MiB)",
		       (unsigned long long)(moov_len >> 20));
		return -1;
	}
	moov = malloc(moov_len);
	if (!moov || pread_full(fd, moov, moov_len, moov_off)) {
		seterr(err, errlen, moov ? "read error in movie header" : "out of memory");
		free(moov);
		return -1;
	}
	if (!box_at(moov, 0, moov_len, &mb)) {
		seterr(err, errlen, "malformed movie header");
		goto out;
	}

	/* first ProRes 422 video track wins; remember why others were refused */
	for (uint64_t toff = mb.body; box_at(moov, toff, mb.end, &trak); toff = trak.end) {
		char terr[512] = "";
		int r;

		if (memcmp(trak.type, "trak", 4))
			continue;
		r = parse_trak(moov, &trak, mi, terr, sizeof(terr));
		if (r == TRAK_OK) {
			rc = 0;
			break;
		}
		if (r != TRAK_SKIP && !first_err[0])
			snprintf(first_err, sizeof(first_err), "%s", terr);
		reset_track(mi);
	}
	if (rc) {
		if (first_err[0])
			seterr(err, errlen, "%s", first_err);
		else if (have_moof)
			seterr(err, errlen, "fragmented MP4 (moof) is not supported — remux with "
			       "ffmpeg -c copy -movflags +faststart");
		else
			seterr(err, errlen, "no video track found");
	}
out:
	free(moov);
	return rc;
}

/* ---- per-frame self-check -------------------------------------------------- */

int mov_selfcheck(int fd, MovInfo *mi, MovProgressFn progress, void *ud,
		  const volatile int *cancel, char *err, size_t errlen)
{
	uint64_t bad = 0, first_bad = 0;
	char first_why[160] = "";
	uint32_t w0 = 0, h0 = 0;

	/* small scattered reads: don't let readahead pull the whole mdat in */
	posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);

	for (uint64_t i = 0; i < mi->n_frames; i++) {
		uint8_t h[FRAME_HDR];
		uint64_t off = mi->offsets[i];
		uint32_t sz = mi->sizes[i];
		char why[160] = "";

		if (cancel && *cancel) {
			seterr(err, errlen, "cancelled");
			return -1;
		}
		if (progress && (i & 255) == 0)
			progress(i, mi->n_frames, ud);

		if (sz < FRAME_HDR || sz > mi->file_size || off > mi->file_size - sz)
			snprintf(why, sizeof(why), "outside the file (off=%llu size=%u)",
				 (unsigned long long)off, sz);
		else if (pread_full(fd, h, sizeof(h), off))
			snprintf(why, sizeof(why), "read error at %llu", (unsigned long long)off);
		else if (be32(h) != sz)
			snprintf(why, sizeof(why), "frame says %u bytes, sample table says %u "
				 "(offset table corrupt?)", be32(h), sz);
		else if (memcmp(h + 4, "icpf", 4))
			snprintf(why, sizeof(why), "no 'icpf' ProRes frame tag");
		else {
			uint32_t w = be16(h + 16), hh = be16(h + 18);
			uint8_t chroma = h[20] >> 6;

			if (i == 0) {
				w0 = w;
				h0 = hh;
				mi->chroma = chroma;
				mi->interlace = (h[20] >> 2) & 3;
				mi->hdr_primaries = h[22];
				mi->hdr_transfer = h[23];
				mi->hdr_matrix = h[24];
				if (!w || !hh || w > DIM_MAX || hh > DIM_MAX)
					snprintf(why, sizeof(why), "bad frame size %ux%u", w, hh);
			} else if (w != w0 || hh != h0) {
				snprintf(why, sizeof(why), "frame size %ux%u differs from first frame %ux%u",
					 w, hh, w0, h0);
			}
			if (!why[0] && chroma != 2)
				snprintf(why, sizeof(why), "chroma format %s, card decodes 4:2:2 only",
					 chroma == 3 ? "4:4:4" : "unknown");
		}
		if (why[0]) {
			if (!bad) {
				first_bad = i;
				snprintf(first_why, sizeof(first_why), "%s", why);
			}
			bad++;
		}
	}
	if (progress)
		progress(mi->n_frames, mi->n_frames, ud);
	posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

	if (bad) {
		seterr(err, errlen, "self-check failed on %llu of %llu frames — not sending "
		       "this file to the card. First bad frame #%llu: %s",
		       (unsigned long long)bad, (unsigned long long)mi->n_frames,
		       (unsigned long long)first_bad, first_why);
		return -1;
	}

	mi->width = w0;
	mi->height = h0;
	mi->out_stride = (w0 * 2 + 63) / 64 * 64;
	mi->out_rows = (h0 + 15) / 16 * 16;
	mi->out_len = mi->out_stride * mi->out_rows;
	mi->in_size = (mi->max_sample + 4095u) & ~4095u;
	return 0;
}

void mov_free(MovInfo *mi)
{
	free(mi->sizes);
	free(mi->offsets);
	mi->sizes = NULL;
	mi->offsets = NULL;
}

/* ---- colour ---------------------------------------------------------------- */

static int norm_matrix(int m)
{
	switch (m) {
	case 1: return 1;
	case 5: case 6: return 6;
	case 9: case 10: return 9;
	default: return 0;
	}
}

int mov_effective_matrix(const MovInfo *mi)
{
	int m = mi->has_colr ? norm_matrix(mi->matrix) : 0;

	if (!m)
		m = norm_matrix(mi->hdr_matrix);
	if (!m)
		m = mi->height >= 720 ? 1 : 6; /* HD and up: BT.709 */
	return m;
}

const char *mov_matrix_name(int m)
{
	switch (m) {
	case 1: return "BT.709";
	case 5: return "BT.601 (BT.470BG)";
	case 6: return "BT.601";
	case 9: return "BT.2020 NCL";
	case 10: return "BT.2020 CL";
	case 2: return "unspecified";
	default: return "unknown";
	}
}

const char *mov_primaries_name(int p)
{
	switch (p) {
	case 1: return "BT.709";
	case 5: return "BT.601 625";
	case 6: return "BT.601 525";
	case 9: return "BT.2020";
	case 11: return "DCI-P3";
	case 12: return "Display P3";
	case 2: return "unspecified";
	default: return "unknown";
	}
}

const char *mov_transfer_name(int t)
{
	switch (t) {
	case 1: return "BT.709";
	case 6: return "BT.601";
	case 13: return "sRGB";
	case 14: case 15: return "BT.2020";
	case 16: return "PQ (ST 2084)";
	case 18: return "HLG";
	case 2: return "unspecified";
	default: return "unknown";
	}
}
