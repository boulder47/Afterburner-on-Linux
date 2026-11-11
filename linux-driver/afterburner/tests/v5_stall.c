// SPDX-License-Identifier: GPL-2.0
/*
 * v5_stall.c — validation matrix V5 (plan.md §8.5): induced stall ->
 * watchdog reset / re-bringup (UC_METHODS §17 sleep_timer port check).
 *
 * The production module ports the kext's AB::sleep_timer hang-watchdog as a
 * synchronous poll-window timeout: no completion within poll_ms -> dev_err +
 * ab_frame_reset() (full kext reset parity) + -ETIMEDOUT. This test induces
 * that path deterministically and safely:
 *
 *   1. submit while the decoder section is DISABLED (post-STOP state):
 *      doorbell rings, firmware has no active decoder -> expect no
 *      completion -> timeout -> reset path exercised. No bad descriptors,
 *      no AER risk — buffers are valid and mapped.
 *   2. IOC_START re-bringup, resubmit the same bound frame with a wide
 *      window -> must complete normally with correct output (recovery).
 *
 * Build: gcc -O2 -o v5_stall v5_stall.c
 * Run:   sudo ./v5_stall <frame.bin> <output_dump.bin> [stall_poll_ms] [recover_poll_ms]
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
#define IOC_STOP         _IO  ('A', 7)
#define IOC_START        _IO  ('A', 8)

int main(int argc, char **argv)
{
	const char *dev = "/dev/afterburner0";
	const char *in_path, *out_path;
	uint32_t stall_ms = 2000, recover_ms = 8000;
	int fd, in_fd, out_fd, ret;
	unsigned char *inmap, *outmap, *buf;
	struct ioc_status st;
	struct ioc_submit s;
	uint32_t sz = 0, in_size, out_len;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <frame.bin> <output_dump.bin> [stall_poll_ms] [recover_poll_ms]\n", argv[0]);
		return 1;
	}
	in_path = argv[1];
	out_path = argv[2];
	if (argc > 3) stall_ms = (uint32_t)strtoul(argv[3], NULL, 10);
	if (argc > 4) recover_ms = (uint32_t)strtoul(argv[4], NULL, 10);

	fd = open(dev, O_RDWR);
	if (fd < 0) { perror("open /dev/afterburner0"); return 1; }

	ret = ioctl(fd, IOC_CARD_STATUS, &st);
	if (ret) { perror("cardStatus"); close(fd); return 1; }
	printf("[status] fw=0x%08x ready=%u dec_enable=0x%x engine_ctrl=0x%x\n",
	       st.fw_version, st.ready, st.dec_enable, st.engine_ctrl);
	if (st.ready) {
		fprintf(stderr, "[abort] decoder is READY — V5 needs the post-STOP state; run abtest_4k first or STOP manually\n");
		close(fd);
		return 1;
	}

	in_fd = open(in_path, O_RDONLY);
	if (in_fd < 0) { perror("open frame"); return 1; }
	while (sz < (uint32_t)(512u << 20)) {
		char tmp[4096];
		ssize_t r = read(in_fd, tmp, sizeof(tmp));

		if (r <= 0) break;
		sz += (uint32_t)r;
	}
	if (!sz) { fprintf(stderr, "empty frame file\n"); return 1; }
	in_size = (sz + 4095) & ~4095u;
	lseek(in_fd, 0, SEEK_SET);

	/* output sized for the firmware's natural padded layout (§35.2) */
	{
		int hf = open(in_path, O_RDONLY);
		uint8_t hdr[32];

		out_len = 122880; /* fallback: 320x180 fixture */
		if (hf >= 0 && read(hf, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)) {
			uint16_t fw = (uint16_t)((hdr[16] << 8) | hdr[17]);
			uint16_t fh = (uint16_t)((hdr[18] << 8) | hdr[19]);

			if (fw && fh) {
				out_len = ((fw * 2u + 63u) / 64u) * 64u * (((fh + 15u) / 16u) * 16u);
				printf("[frame] header says %ux%u — out_len=%u\n", fw, fh, out_len);
			}
		}
		close(hf);
	}

	struct ioc_bind b = { in_size, out_len };
	ret = ioctl(fd, IOC_BIND_FRAME, &b);
	if (ret) { perror("bindFrame"); return 1; }

	inmap = mmap(NULL, in_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	outmap = mmap(NULL, out_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)1 * 4096);
	if (inmap == MAP_FAILED || outmap == MAP_FAILED) { perror("mmap"); return 1; }

	buf = malloc(in_size);
	if (!buf) { fprintf(stderr, "oom\n"); return 1; }
	{
		uint32_t got = 0;

		while (got < sz) {
			ssize_t r = read(in_fd, buf + got, sz - got);

			if (r <= 0) break;
			got += (uint32_t)r;
		}
		if (got != sz) { fprintf(stderr, "short read\n"); return 1; }
	}
	memcpy(inmap, buf, sz);
	free(buf);
	memset(outmap, 0x5A, out_len);

	/* ---- Phase 1: induced stall — submit with decoder DISABLED ---- */
	s.in_off = 0;
	s.in_len = sz;
	s.out_len = out_len;
	s.poll_ms = stall_ms;
	printf("[stall] submitting with decoder DISABLED poll_ms=%u ...\n", stall_ms);
	ret = ioctl(fd, IOC_SUBMIT_FRAME, &s);
	if (ret) {
		int e = errno;

		printf("[stall] submit failed: %d (%s)\n", -e, strerror(e));
		if (e != ETIMEDOUT) {
			fprintf(stderr, "[FAIL] expected -ETIMEDOUT from the disabled-decoder stall (got %s — driver guard may reject pre-START submits; see ENODEV path in ab_dma.c)\n", strerror(e));
			return 1;
		}
		printf("[stall] OK — timeout path fired (sleep_timer port: reset on hang)\n");
	} else {
		fprintf(stderr, "[!!] decoder-off submit COMPLETED (status=0x%08x data=0x%x) — no stall induced; V5 needs another vector\n",
			s.status, s.data_word);
		return 2;
	}

	/* ---- Phase 2: re-bringup + recovery decode --------------------- */
	printf("[recover] issuing IOC_START ...\n");
	ret = ioctl(fd, IOC_START, NULL);
	if (ret) { perror("startFrame"); return 1; }
	ioctl(fd, IOC_CARD_STATUS, &st);
	printf("[recover] fw=0x%08x ready=%u dec_enable=0x%x\n",
	       st.fw_version, st.ready, st.dec_enable);
	if (!st.ready) { fprintf(stderr, "decoder not ready after START — aborting\n"); return 1; }

	memset(outmap, 0x5A, out_len); /* fresh sentinel for the recovery decode */
	s.in_off = 0;
	s.in_len = sz;
	s.out_len = out_len;
	s.poll_ms = recover_ms;
	printf("[recover] resubmitting poll_ms=%u ...\n", recover_ms);
	ret = ioctl(fd, IOC_SUBMIT_FRAME, &s);
	if (ret) { perror("submitFrame-recover"); return 1; }
	printf("[recover] OK status=0x%08x data_word=0x%08x\n", s.status, s.data_word);

	out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out_fd < 0) { perror("open output dump"); return 1; }
	{
		uint32_t done = 0;

		while (done < out_len) {
			ssize_t w = write(out_fd, outmap + done, out_len - done);

			if (w <= 0) break;
			done += (uint32_t)w;
		}
		printf("[dump] %s: %u bytes\n", out_path, done);
	}
	close(out_fd);

	ret = ioctl(fd, IOC_STOP, NULL);
	if (ret) { perror("stop"); return 1; }
	ioctl(fd, IOC_CARD_STATUS, &st);
	printf("[post-stop] ready=%u dec_enable=0x%x engine_ctrl=0x%x\n",
	       st.ready, st.dec_enable, st.engine_ctrl);
	printf(st.ready || st.dec_enable ? "[!!] STOP did not quiesce the decoder\n"
	                                 : "[ok] decoder disabled — card idle for AER watch\n");

	ioctl(fd, IOC_UNBIND_ALL, NULL);
	close(in_fd);
	close(fd);
	return 0;
}
