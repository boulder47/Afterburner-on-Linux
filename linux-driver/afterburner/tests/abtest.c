// SPDX-License-Identifier: GPL-2.0
/*
 * abtest.c — Phase 3u.2 userspace smoke test (validation matrix V1).
 *
 * Exercises the full /dev/afterburner0 path against a real card:
 *   open -> cardStatus -> bindFrame -> mmap in/out -> submitFrame(ProRes 320x180)
 *        -> queryCompletion -> checkForSpace -> getPercent -> unbindAll -> close
 *
 * Build: gcc -O2 -o abtest abtest.c
 * Run:   sudo ./abtest /path/to/prores_320x180_frame0.bin
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/types.h>

/* --- mirror of ab_chardev.c ioctl ABI (keep in sync) ---------------- */
struct ioc_submit {
	__u32 in_off, in_len, out_len, poll_ms;
	__u32 status, data_word;
};
struct ioc_query  { __u32 status, data_word; };
struct ioc_bind   { __u32 in_size, out_size; };
struct ioc_status { __u32 fw_version, ready, dec_enable, engine_ctrl; };
struct ioc_space  { __u32 in_flight, max_in_flight; };

#define IOC_SUBMIT_FRAME _IOWR('A', 0, struct ioc_submit)
#define IOC_QUERY_COMPL  _IOR ('A', 1, struct ioc_query)
#define IOC_BIND_FRAME   _IOWR('A', 2, struct ioc_bind)
#define IOC_CARD_STATUS  _IOR ('A', 3, struct ioc_status)
#define IOC_CHECK_SPACE  _IOR ('A', 4, struct ioc_space)
#define IOC_UNBIND_ALL   _IO  ('A', 5)
#define IOC_GET_PERCENT  _IOR ('A', 6, __u32)

static void hexdump(const char *tag, const unsigned char *p, size_t n)
{
	size_t i;
	printf("%s: ", tag);
	for (i = 0; i < n && i < 64; i++)
		printf("%02x", p[i]);
	if (n > 64)
		printf("...");
	printf("\n");
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/afterburner0";
	const char *fixture;
	int fd, ret;
	unsigned char *inmap, *outmap;
	struct ioc_status st;
	struct ioc_bind b;
	struct ioc_submit s;
	struct ioc_query q;
	struct ioc_space sp;
	__u32 pct = 0xffffffff;
	size_t in_len, out_len, nz, i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <prores-fixture.bin>\n", argv[0]);
		return 2;
	}
	fixture = argv[1];

	fd = open(dev, O_RDWR);
	if (fd < 0) { perror("open /dev/afterburner0"); return 1; }
	printf("[ok] opened %s\n", dev);

	ret = ioctl(fd, IOC_CARD_STATUS, &st);
	if (ret) { perror("cardStatus"); close(fd); return 1; }
	printf("[status] fw=0x%08x ready=%u dec_enable=0x%x engine_ctrl=0x%x\n",
	       st.fw_version, st.ready, st.dec_enable, st.engine_ctrl);
	if (!st.ready) { fprintf(stderr, "decoder not ready — aborting\n"); close(fd); return 1; }

	/* load fixture */
	int ff = open(fixture, O_RDONLY);
	if (ff < 0) { perror("open fixture"); close(fd); return 1; }
	in_len = lseek(ff, 0, SEEK_END);
	lseek(ff, 0, SEEK_SET);
	unsigned char *frame = malloc(in_len);
	if (!frame || read(ff, frame, in_len) != (ssize_t)in_len) {
		fprintf(stderr, "read fixture failed\n"); return 1;
	}
	close(ff);
	printf("[fixture] %s: %zu bytes\n", fixture, in_len);

	/* bindFrame — input region must hold the frame; output = w*h*2 (YUV422) */
	out_len = 320u * 180u * 2u;
	b.in_size = 65536;
	b.out_size = 131072;
	ret = ioctl(fd, IOC_BIND_FRAME, &b);
	if (ret) { perror("bindFrame"); return 1; }
	printf("[ok] bindFrame in=%u out=%u\n", b.in_size, b.out_size);

	inmap = mmap(NULL, b.in_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	outmap = mmap(NULL, b.out_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)1 * 4096);
	if (inmap == MAP_FAILED || outmap == MAP_FAILED) {
		perror("mmap"); return 1;
	}
	printf("[ok] mmap in/out\n");

	memset(outmap, 0, b.out_size);
	memcpy(inmap, frame, in_len);

	/* multi-frame loop (default 3): same input every time -> deterministic
	 * decoder must produce byte-identical output on each pass. */
	int nframes = (argc > 2) ? atoi(argv[2]) : 3;
	const char *dump_prefix = (argc > 3) ? argv[3] : NULL;
	unsigned long settle_us = getenv("SETTLE_MS") ? atol(getenv("SETTLE_MS")) * 1000 : 0;
	unsigned char *ref = malloc(out_len);
	int fails = 0, first_ok = -1;

	/* input-integrity check: does the card ever modify our input region? */
	unsigned char *in_copy = malloc(in_len);
	memcpy(in_copy, inmap, in_len);

	s.in_off = 0;
	s.in_len = (uint32_t)in_len;
	s.out_len = (uint32_t)out_len;
	s.poll_ms = 3000;

	for (int f = 0; f < nframes; f++) {
		memset(outmap, 0x5A, b.out_size); /* sentinel: untouched by c2h */
		ret = ioctl(fd, IOC_SUBMIT_FRAME, &s);
		if (ret) {
			perror("submitFrame");
			printf("[FAIL] frame %d: submit returned %d\n", f, ret);
			fails++;
			continue;
		}
		usleep(settle_us);
		if (memcmp(inmap, in_copy, in_len)) {
			size_t d = 0;
			while (d < in_len && inmap[d] == in_copy[d]) d++;
			printf("[!!] INPUT REGION MODIFIED after frame %d: first diff at offset %zu\n", f, d);
		}
		nz = 0;
		for (i = 0; i < out_len; i++)
			if (outmap[i]) nz++;
		/* report sentinel (0x5A) runs: regions c2h did NOT write this frame */
		{
			size_t start = SIZE_MAX, runlen = 0, printed = 0;

			for (i = 0; i <= out_len; i++) {
				if (i < out_len && outmap[i] == 0x5A) {
					if (start == SIZE_MAX) start = i;
					runlen++;
				} else if (runlen) {
					if (printed < 8)
						printf("    sentinel run: [%zu, %zu) len=%zu\n",
						       start, start + runlen, runlen);
					printed++;
					runlen = 0;
				}
			}
			if (printed > 8)
				printf("    ... %zu more sentinel runs\n", printed - 8);
		}
		printf("[frame %d] status=0x%08x data=0x%08x output=%zu/%zu nonzero (%.1f%%)\n",
		       f, s.status, s.data_word, nz, out_len, 100.0 * nz / (double)out_len);
		if (dump_prefix) {
			char path[256];
			snprintf(path, sizeof(path), "%s.f%d.yuv", dump_prefix, f);
			FILE *df = fopen(path, "wb");
			if (df) { fwrite(outmap, 1, out_len, df); fclose(df); }
		}
		if (nz < out_len / 2) {
			printf("[FAIL] frame %d: output mostly empty\n", f);
			fails++;
			continue;
		}
		if (first_ok < 0) {
			first_ok = f;
			memcpy(ref, outmap, out_len);
			hexdump("first64", outmap, 64);
		} else if (memcmp(outmap, ref, out_len)) {
			size_t first_diff = 0, nzref = 0, nzn = 0;
			while (first_diff < out_len && outmap[first_diff] == ref[first_diff])
				first_diff++;
			for (i = 0; i < out_len; i++) { if (outmap[i]) nzn++; if (ref[i]) nzref++; }
			printf("[FAIL] frame %d: differs from frame %d at offset %zu (this=%02x ref=%02x); nonzero this=%zu ref=%zu\n",
			       f, first_ok, first_diff, outmap[first_diff], ref[first_diff], nzn, nzref);
			fails++;
		} else {
			printf("[ok] frame %d: identical to reference frame %d\n", f, first_ok);
		}
	}

	if (fails)
		printf("=== %d/%d frames FAILED ===\n", fails, nframes);
	else
		printf("=== all %d frames OK ===\n", nframes);
	free(ref);

	ret = ioctl(fd, IOC_QUERY_COMPL, &q);
	printf("[query] ret=%d status=0x%08x data=0x%08x\n", ret, q.status, q.data_word);

	ret = ioctl(fd, IOC_CHECK_SPACE, &sp);
	printf("[space] in_flight=%u/%u (ret=%d)\n", sp.in_flight, sp.max_in_flight, ret);

	ret = ioctl(fd, IOC_GET_PERCENT, &pct);
	printf("[percent] %u (ret=%d)\n", pct, ret);

	ret = ioctl(fd, IOC_UNBIND_ALL, NULL);
	printf("[ok] unbindAll (ret=%d)\n", ret);

	close(fd);
	printf("=== done ===\n");
	return 0;
}
