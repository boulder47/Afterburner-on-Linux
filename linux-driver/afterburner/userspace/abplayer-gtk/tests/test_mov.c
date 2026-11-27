// SPDX-License-Identifier: GPL-2.0
/*
 * test_mov.c — parser + self-check driver for tests/run-tests.sh.
 *
 *   test_mov FILE                 parse + self-check, print a summary, exit 0/1
 *   test_mov --expect K=V... FILE also assert fields (fourcc, w, h, n, fps, matrix)
 *   test_mov --fuzz N SEED FILE   mutate the file's bytes N times in memory
 *                                 (via memfd) and parse each copy; must not
 *                                 crash (run under ASan/UBSan)
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/mov.h"

static int check_file(int fd, int verbose, MovInfo *mi, char *err, size_t n)
{
	if (mov_parse(fd, mi, err, n))
		return -1;
	if (mov_selfcheck(fd, mi, NULL, NULL, NULL, err, n)) {
		mov_free(mi);
		return -1;
	}
	if (verbose)
		printf("OK %s %s %ux%u frames=%llu fps=%.3f delta=%u ts=%u max=%u in=%u out=%u "
		       "stride=%u rows=%u matrix=%s colr=%d(%u/%u/%u) hdr=%u/%u/%u par=%u:%u il=%u\n",
		       mi->fourcc, mi->profile, mi->width, mi->height,
		       (unsigned long long)mi->n_frames, mi->fps, mi->frame_delta, mi->timescale,
		       mi->max_sample, mi->in_size, mi->out_len, mi->out_stride, mi->out_rows,
		       mov_matrix_name(mov_effective_matrix(mi)), mi->has_colr, mi->primaries,
		       mi->transfer, mi->matrix, mi->hdr_primaries, mi->hdr_transfer,
		       mi->hdr_matrix, mi->par_num, mi->par_den, mi->interlace);
	return 0;
}

static int fuzz(const char *path, long iters, unsigned seed)
{
	int fd = open(path, O_RDONLY);
	struct stat st;
	uint8_t *orig;
	long ok = 0, rej = 0;

	if (fd < 0 || fstat(fd, &st)) {
		perror(path);
		return 2;
	}
	orig = malloc(st.st_size);
	if (!orig || pread(fd, orig, st.st_size, 0) != st.st_size) {
		perror("read");
		return 2;
	}
	close(fd);
	srand(seed);

	/* bias mutations towards the moov box, where the parser lives */
	size_t moov = 0;
	for (size_t i = 4; i + 4 <= (size_t)st.st_size; i++)
		if (!memcmp(orig + i, "moov", 4)) {
			moov = i - 4;
			break;
		}

	for (long it = 0; it < iters; it++) {
		int mfd = memfd_create("fuzz", 0);
		size_t len = st.st_size;
		uint8_t *buf = malloc(len);
		MovInfo mi;
		char err[512];

		memcpy(buf, orig, len);
		int nmut = 1 + rand() % 8;
		for (int k = 0; k < nmut; k++) {
			if (!len)
				break;
			size_t pos = (moov && moov < len && rand() % 4)
					     ? moov + (size_t)rand() % (len - moov)
					     : (size_t)rand() % len;
			switch (rand() % 4) {
			case 0: buf[pos] = (uint8_t)rand(); break;
			case 1: buf[pos] ^= (uint8_t)(1u << (rand() % 8)); break;
			case 2: /* 32-bit boundary value */
				if (pos + 4 <= len) {
					static const uint32_t v[] = { 0, 1, 0xffffffffu, 0x7fffffffu, 8, 16 };
					uint32_t x = v[rand() % 6];
					buf[pos] = x >> 24; buf[pos + 1] = x >> 16;
					buf[pos + 2] = x >> 8; buf[pos + 3] = x;
				}
				break;
			case 3: /* truncate */
				if (rand() % 8 == 0)
					len = pos;
				break;
			}
		}
		if (write(mfd, buf, len) != (ssize_t)len) {
			perror("memfd write");
			return 2;
		}
		if (check_file(mfd, 0, &mi, err, sizeof(err)) == 0) {
			ok++;
			mov_free(&mi);
		} else {
			rej++;
		}
		close(mfd);
		free(buf);
	}
	printf("fuzz: %ld iterations, %ld parsed OK, %ld rejected, no crash\n", iters, ok, rej);
	free(orig);
	return 0;
}

int main(int argc, char **argv)
{
	const char *exp_fourcc = NULL;
	long exp_w = -1, exp_h = -1, exp_n = -1, exp_matrix = -1;
	double exp_fps = -1;
	int expect_fail = 0;
	int i = 1;

	if (argc >= 5 && !strcmp(argv[1], "--fuzz"))
		return fuzz(argv[4], atol(argv[2]), (unsigned)atol(argv[3]));

	for (; i < argc - 1; i++) {
		char *a = argv[i];

		if (!strncmp(a, "fourcc=", 7)) exp_fourcc = a + 7;
		else if (!strncmp(a, "w=", 2)) exp_w = atol(a + 2);
		else if (!strncmp(a, "h=", 2)) exp_h = atol(a + 2);
		else if (!strncmp(a, "n=", 2)) exp_n = atol(a + 2);
		else if (!strncmp(a, "fps=", 4)) exp_fps = atof(a + 4);
		else if (!strncmp(a, "matrix=", 7)) exp_matrix = atol(a + 7);
		else if (!strcmp(a, "--expect-fail")) expect_fail = 1;
		else if (!strcmp(a, "--expect")) ;
		else { fprintf(stderr, "unknown arg %s\n", a); return 2; }
	}
	if (i >= argc) {
		fprintf(stderr, "usage: test_mov [--expect k=v...] [--expect-fail] FILE\n");
		return 2;
	}

	int fd = open(argv[i], O_RDONLY);
	if (fd < 0) {
		perror(argv[i]);
		return 2;
	}
	MovInfo mi;
	char err[512];
	int r = check_file(fd, 1, &mi, err, sizeof(err));
	close(fd);

	if (expect_fail) {
		if (r == 0) {
			printf("FAIL: %s parsed but was expected to be rejected\n", argv[i]);
			return 1;
		}
		printf("REJECTED (expected): %s\n", err);
		return 0;
	}
	if (r) {
		printf("FAIL: %s: %s\n", argv[i], err);
		return 1;
	}
	int bad = 0;
	if (exp_fourcc && strcmp(exp_fourcc, mi.fourcc)) { printf("FAIL fourcc %s != %s\n", mi.fourcc, exp_fourcc); bad++; }
	if (exp_w >= 0 && exp_w != mi.width) { printf("FAIL w %u != %ld\n", mi.width, exp_w); bad++; }
	if (exp_h >= 0 && exp_h != mi.height) { printf("FAIL h %u != %ld\n", mi.height, exp_h); bad++; }
	if (exp_n >= 0 && (unsigned long long)exp_n != mi.n_frames) { printf("FAIL n %llu != %ld\n", (unsigned long long)mi.n_frames, exp_n); bad++; }
	if (exp_fps >= 0 && fabs(exp_fps - mi.fps) > 0.01) { printf("FAIL fps %.3f != %.3f\n", mi.fps, exp_fps); bad++; }
	if (exp_matrix >= 0 && exp_matrix != mov_effective_matrix(&mi)) { printf("FAIL matrix %d != %ld\n", mov_effective_matrix(&mi), exp_matrix); bad++; }
	mov_free(&mi);
	return bad ? 1 : 0;
}
