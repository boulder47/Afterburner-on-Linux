// SPDX-License-Identifier: GPL-2.0
/*
 * abtest_4k.c — validation matrix V3 (plan.md §8.5): 4K H1/H2 test on the
 * production module, mode-14 equivalent with corrected output size.
 *
 * Submits tmp/p4k_frame0.bin (raw ProRes 4K frame, byte-identical to sample 0
 * of tmp/p4k_f0.mov — verified) through the two-stage path and dumps the full
 * decoded output for comparison against the FFmpeg reference:
 *   ffmpeg -i p4k_f0.mov -frames:v 1 -pix_fmt yuv422p -f rawvideo ref.yuv
 * (card layout recipe + compare: tests/v3_compare.py)
 *
 * H1 vs H2 (§27.5): with out_seg=4MB the receive chain is 4 descriptors for a
 * 16,588,800 B output — well under both the ~126-desc count cap (H1) and any
 * ~16 MiB byte cap (H2). A full-frame decode therefore confirms H1 semantics;
 * a partial/missing tail would implicate a byte-cap. Run with:
 *   insmod afterburner.ko out_seg=4194304     # 4 MB fixed segments
 *   sudo ./abtest_4k tmp/p4k_frame0.bin /tmp/v3_out.bin 30000
 * then IOC_STOP leaves the decoder disabled for the >=75 s idle AER watch.
 *
 * Build: gcc -O2 -o abtest_4k abtest_4k.c
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

/* V3 fixture: ProRes 4K (3840x2160). Card output = interleaved YUV422 8-bit,
 * w*h*2 bytes (§32 / §26.2 — NOT planar p10le as the original plan assumed). */
#define W 3840u
#define H 2160u
static uint32_t out_len = W * H * 2; /* default; re-parsed from frame header */

int main(int argc, char **argv)
{
	const char *dev = "/dev/afterburner0";
	const char *in_path, *out_path;
	uint32_t poll_ms = 30000;
	int fd, in_fd, out_fd, ret;
	unsigned char *inmap, *outmap, *buf;
	struct ioc_status st;
	struct ioc_submit s;
	uint32_t sz = 0, in_size;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <frame.bin> <output_dump.bin> [poll_ms] [out_seg_bytes] [out_len]\n", argv[0]);
		return 1;
	}
	in_path = argv[1];
	out_path = argv[2];
	if (argc > 3)
		poll_ms = (uint32_t)strtoul(argv[3], NULL, 10);

	fd = open(dev, O_RDWR);
	if (fd < 0) { perror("open /dev/afterburner0"); return 1; }

	/* optional: set the module's descriptor segment size before submit
	 * (we are root — no extra sudo needed for the sysfs write) */
	if (argc > 4) {
		uint32_t seg = (uint32_t)strtoul(argv[4], NULL, 10);
		FILE *f = fopen("/sys/module/afterburner/parameters/out_seg", "w");

		if (!f) { perror("open out_seg sysfs"); close(fd); return 1; }
		fprintf(f, "%u\n", seg);
		fclose(f);
		printf("[seg] out_seg=%u\n", seg);
	}

	ret = ioctl(fd, IOC_CARD_STATUS, &st);
	if (ret) { perror("cardStatus"); close(fd); return 1; }
	printf("[status] fw=0x%08x ready=%u dec_enable=0x%x engine_ctrl=0x%x\n",
	       st.fw_version, st.ready, st.dec_enable, st.engine_ctrl);
	if (!st.ready) {
		/* decoder disabled by a previous run's STOP — re-bring-up (kext
		 * recovery path, mode-8 rung A) instead of requiring rmmod/insmod */
		printf("[start] decoder not ready — issuing IOC_START ...\n");
		ret = ioctl(fd, IOC_START, NULL);
		if (ret) { perror("startFrame"); close(fd); return 1; }
		ioctl(fd, IOC_CARD_STATUS, &st);
		printf("[start] fw=0x%08x ready=%u dec_enable=0x%x\n",
		       st.fw_version, st.ready, st.dec_enable);
		if (!st.ready) { fprintf(stderr, "decoder not ready after START — aborting\n"); close(fd); return 1; }
	}

	in_fd = open(in_path, O_RDONLY);
	if (in_fd < 0) { perror("open frame"); return 1; }
	{
		char tmp[4096];

		while (sz < (uint32_t)(512u << 20)) { /* sanity cap; ProRes frames are small */
			ssize_t r = read(in_fd, tmp, sizeof(tmp));

			if (r <= 0)
				break;
			sz += (uint32_t)r;
		}
		if (!sz) { fprintf(stderr, "empty frame file\n"); return 1; }
		in_size = (sz + 4095) & ~4095u; /* page-align the bind size */
		lseek(in_fd, 0, SEEK_SET);
	}
	printf("[frame] %s: %u bytes (bind in=%u out=%u)\n", in_path, sz, in_size, out_len);

	/* parse W/H from the ProRes frame header (u16BE at +16/+18 of the
	 * 'icpf' box) so non-4K fixtures work; size the output buffer for the
	 * firmware's NATURAL padded layout (§35.2): interleaved [U,Y0,V,Y1] with
	 * row stride = ceil(W*2/64)*64 and rows = ceil(H/16)*16 (padding rows are
	 * copies of the last real row). W*H*2 truncates real content whenever
	 * W%32 != 0. */
	{
		int hf = open(in_path, O_RDONLY);
		uint8_t hdr[32];

		if (hf >= 0 && read(hf, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)) {
			uint16_t fw = (uint16_t)((hdr[16] << 8) | hdr[17]);
			uint16_t fh = (uint16_t)((hdr[18] << 8) | hdr[19]);

			if (fw && fh) {
				uint32_t stride = ((fw * 2u + 63u) / 64u) * 64u;
				uint32_t rows = ((fh + 15u) / 16u) * 16u;

				out_len = stride * rows;
				printf("[frame] header says %ux%u — stride=%u rows=%u out_len=%u\n",
				       fw, fh, stride, rows, out_len);
			}
		}
		close(hf);
	}

	/* AB_OUT_MULT=<factor> — scale out_len for output-layout experiments
	 * (e.g. 2.0 = v216-sized buffer test; FINDINGS §35 follow-up: does the
	 * firmware select its output layout from the bound buffer size?) */
	{
		const char *m = getenv("AB_OUT_MULT");

		if (m && atof(m) > 0.0) {
			out_len = (uint32_t)((double)out_len * atof(m));
			printf("[frame] AB_OUT_MULT=%s — out_len scaled to %u\n", m, out_len);
		}
	}

	/* optional: explicit out_len override (layout experiments; e.g. the
	 * unpadded W*H*2 size used in FINDINGS §32) */
	if (argc > 5) {
		out_len = (uint32_t)strtoul(argv[5], NULL, 10);
		printf("[frame] out_len overridden to %u\n", out_len);
	}

	struct ioc_bind b = { in_size, out_len };
	ret = ioctl(fd, IOC_BIND_FRAME, &b);
	if (ret) { perror("bindFrame"); return 1; }

	inmap = mmap(NULL, in_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	outmap = mmap(NULL, out_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)1 * 4096);
	if (inmap == MAP_FAILED || outmap == MAP_FAILED) { perror("mmap"); return 1; }

	/* load the frame into the mapped input region */
	buf = malloc(in_size);
	if (!buf) { fprintf(stderr, "oom\n"); return 1; }
	{
		uint32_t got = 0;

		while (got < sz) {
			ssize_t r = read(in_fd, buf + got, sz - got);

			if (r <= 0) break;
			got += (uint32_t)r;
		}
		if (got != sz) { fprintf(stderr, "short read: %u/%u\n", got, sz); return 1; }
	}
	memcpy(inmap, buf, sz);
	free(buf);

	/* sentinel: any region the c2h DMA never touches stays 0x5A — that is how
	 * a partial decode (e.g. an H2 byte-cap skipping tail descriptors) shows up */
	memset(outmap, 0x5A, out_len);

	s.in_off = 0;
	s.in_len = sz;
	s.out_len = out_len;
	s.poll_ms = poll_ms;

	printf("[submit] in_len=%u out_len=%u poll_ms=%u ...\n", s.in_len, s.out_len, poll_ms);
	ret = ioctl(fd, IOC_SUBMIT_FRAME, &s);
	if (ret) {
		perror("submitFrame");
		printf("[FAIL] submit returned %d (%s)\n", ret, strerror(-ret));
		return 1;
	}
	printf("[submit] OK status=0x%08x data_word=0x%08x\n", s.status, s.data_word);

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

	/* V3 stop block: decoder disabled for the idle AER watch. Set
	 * AB_NOSTOP=1 to leave the decoder enabled (segment-size iteration). */
	if (getenv("AB_NOSTOP")) {
		printf("[skip] AB_NOSTOP set — decoder left enabled\n");
	} else {
		ret = ioctl(fd, IOC_STOP, NULL);
		if (ret) { perror("stop"); return 1; }
		ioctl(fd, IOC_CARD_STATUS, &st);
		printf("[post-stop] ready=%u dec_enable=0x%x engine_ctrl=0x%x\n",
		       st.ready, st.dec_enable, st.engine_ctrl);
		printf(st.ready || st.dec_enable ? "[!!] STOP did not quiesce the decoder\n"
		                                 : "[ok] decoder disabled — card idle for AER watch\n");
	}

	ioctl(fd, IOC_UNBIND_ALL, NULL);
	close(in_fd);
	close(fd);
	return 0;
}
