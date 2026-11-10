// SPDX-License-Identifier: GPL-2.0
/*
 * abtest_v1.c — validation matrix V1 (plan.md §8.5): len=64 single-shot + VALID ack,
 * regression baseline vs FINDINGS §21.7(a) (Phase 3i mode=1/3 on the old test module).
 *
 * Submits a 64-byte input through the production two-stage path:
 *   in chain = ONE descriptor with desc[+0x04]=64 (kext fn@0x9c5c layout)
 *   out chain = one page, engine acks any payload (§3k mode=5).
 *
 * PASS criteria (per §21.7(a)): submit completes (ret==0), final status word has
 * bit0 set (VALID), data word == 6 (normal completion path — the kext handler's
 * only test of `data` is bit0; 6 = 0b110, bit0 clear).
 *
 * Build: gcc -O2 -o abtest_v1 abtest_v1.c
 * Run:   sudo ./abtest_v1
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
struct ioc_bind   { __u32 in_size, out_size; };
struct ioc_status { __u32 fw_version, ready, dec_enable, engine_ctrl; };

#define IOC_SUBMIT_FRAME _IOWR('A', 0, struct ioc_submit)
#define IOC_BIND_FRAME   _IOWR('A', 2, struct ioc_bind)
#define IOC_CARD_STATUS  _IOR ('A', 3, struct ioc_status)
#define IOC_UNBIND_ALL   _IO  ('A', 5)

int main(void)
{
	const char *dev = "/dev/afterburner0";
	int fd, ret;
	unsigned char *inmap, *outmap;
	struct ioc_status st;
	struct ioc_submit s;
	uint32_t in_size = 4096, out_size = 4096;

	fd = open(dev, O_RDWR);
	if (fd < 0) { perror("open /dev/afterburner0"); return 1; }

	ret = ioctl(fd, IOC_CARD_STATUS, &st);
	if (ret) { perror("cardStatus"); close(fd); return 1; }
	printf("[status] fw=0x%08x ready=%u dec_enable=0x%x engine_ctrl=0x%x\n",
	       st.fw_version, st.ready, st.dec_enable, st.engine_ctrl);
	if (!st.ready) { fprintf(stderr, "decoder not ready — aborting\n"); close(fd); return 1; }

	/* bindFrame: one page each side is enough for a 64-byte submit */
	struct ioc_bind b = { in_size, out_size };
	ret = ioctl(fd, IOC_BIND_FRAME, &b);
	if (ret) { perror("bindFrame"); return 1; }

	inmap = mmap(NULL, in_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	outmap = mmap(NULL, out_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)1 * 4096);
	if (inmap == MAP_FAILED || outmap == MAP_FAILED) { perror("mmap"); return 1; }

	/* recognizable input pattern — also lets us verify the card never
	 * writes back into our input region */
	for (int i = 0; i < 64; i++)
		inmap[i] = (unsigned char)i;
	unsigned char in_copy[64];
	memcpy(in_copy, inmap, 64);
	memset(outmap, 0x5A, out_size);

	s.in_off = 0;
	s.in_len = 64;
	s.out_len = 4096;
	s.poll_ms = 3000;

	ret = ioctl(fd, IOC_SUBMIT_FRAME, &s);
	if (ret) {
		perror("submitFrame");
		printf("[FAIL] submit returned %d (%s)\n", ret, strerror(-ret));
		return 1;
	}
	printf("[submit] ret=0 status=0x%08x data_word=0x%08x\n", s.status, s.data_word);

	if (memcmp(inmap, in_copy, 64)) {
		size_t d = 0;
		while (d < 64 && inmap[d] == in_copy[d]) d++;
		printf("[!!] INPUT REGION MODIFIED: first diff at %zu\n", d);
	} else {
		printf("[ok] input region untouched\n");
	}

	int pass = (s.status & 1) && s.data_word == 6;
	printf(pass ? "=== V1 PASS: len=64 consumed, status VALID, data=6 (§21.7(a) baseline) ===\n"
	                : "=== V1 CHECK: completion arrived but words differ from §21.7(a) baseline — inspect dmesg ===\n");

	ioctl(fd, IOC_UNBIND_ALL, NULL);
	close(fd);
	return 0;
}
