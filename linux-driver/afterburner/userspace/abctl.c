// SPDX-License-Identifier: GPL-2.0
/*
 * abctl.c — Apple Afterburner control client (plan2.md Phase A).
 *
 * The supported userspace interface for /dev/afterburnerN + the sysfs surface.
 * Mirrors the UC sMethods ABI exposed by ab_chardev.c (keep in sync):
 *   submitFrame / queryCompletion / bindFrame / cardStatus / checkForSpace /
 *   unbindAll / getPercent / stop / start
 *
 * Subcommands:
 *   abctl status                     card state + thermal snapshot
 *   abctl decode <frame> [out.yuv] [--poll N] [--nostop]
 *                                    full round-trip: bind -> mmap -> submit -> dump.
 *                                    Auto-parses W/H from the ProRes frame header and
 *                                    sizes output per the firmware's padded layout
 *                                    (FINDINGS §35.2). Stops the decoder by default —
 *                                    a full-frame decode + enabled decoder + long idle is
 *                                    the V6 kill state (§28/§33); --nostop for active
 *                                    multi-frame workloads only.
 *   abctl start                      re-bring-up after stop (IOC_START)
 *   abctl stop                       disable decoder (kext stop() parity)
 *   abctl unbind                     release bound DMA regions (IOC_UNBIND_ALL)
 *
 * Build: gcc -O2 -o abctl abctl.c
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
struct ioc_space  { __u32 in_flight, max_in_flight;
                    __u64 used_bytes, cap_bytes; }; /* v2: kext reserve model */
struct ioc_async  { __u32 in_off, in_len, out_len; };

#define IOC_SUBMIT_FRAME _IOWR('A', 0, struct ioc_submit)
#define IOC_QUERY_COMPL  _IOR ('A', 1, struct ioc_query)
#define IOC_BIND_FRAME   _IOWR('A', 2, struct ioc_bind)
#define IOC_CARD_STATUS  _IOR ('A', 3, struct ioc_status)
#define IOC_CHECK_SPACE  _IOR ('A', 4, struct ioc_space)
#define IOC_UNBIND_ALL   _IO  ('A', 5)
#define IOC_GET_PERCENT  _IOR ('A', 6, __u32)
#define IOC_STOP         _IO  ('A', 7)
#define IOC_START        _IO  ('A', 8)
#define IOC_SUBMIT_ASYNC _IOWR('A', 9, struct ioc_async)
#define IOC_WAIT_COMPL   _IOR ('A',10, struct ioc_query)
#define IOC_RESET        _IO  ('A',11)

static const char *dev_path = "/dev/afterburner0";

static void die(const char *what)
{
	perror(what);
	exit(1);
}

/* ------------------------------------------------------------------ */
/* status                                                              */
static int cmd_status(void)
{
	struct ioc_status st;
	int fd, ret;

	fd = open(dev_path, O_RDWR);
	if (fd < 0)
		die("open /dev/afterburner0");
	ret = ioctl(fd, IOC_CARD_STATUS, &st);
	if (ret) {
		close(fd);
		die("cardStatus");
	}

	printf("fw_version   : %u.%u (raw 0x%08x)\n",
	       (st.fw_version >> 16) & 0xFFFF, st.fw_version & 0xFFFF, st.fw_version);
	printf("decoder      : ready=%u enable=0x%x\n", st.ready, st.dec_enable);
	printf("engine_ctrl  : 0x%08x   completion: see sysfs 'completion'\n", st.engine_ctrl);

	/* space pool (kext bindFrame reserve model, UC_METHODS §6) */
	struct ioc_space sp;
	if (!ioctl(fd, IOC_CHECK_SPACE, &sp)) {
		printf("space        : in_flight=%u/%u  used=%.1f MiB / %.0f MiB\n",
		       sp.in_flight, sp.max_in_flight,
		       (double)sp.used_bytes / (1 << 20),
		       (double)sp.cap_bytes / (1 << 20));
	} else {
		printf("space        : CHECK_SPACE failed (%m)\n");
	}
	close(fd);

	/* thermal snapshot from the sysfs surface (Phase A) */
	const char *base = "/sys/class/afterburner/afterburner0";
	char path[256];
	FILE *f;
	int t, got = 0;

	printf("thermal      :\n");
#define READ_TEMP(name, label) \
	snprintf(path, sizeof(path), "%s/%s", base, name); \
	f = fopen(path, "r"); \
	if (f && fscanf(f, "%d", &t) == 1) { printf("%-13s: %d C\n", label, t); got++; } \
	if (f) fclose(f);

	READ_TEMP("temp_backcard_c", "backcard")
	READ_TEMP("temp_fpga_die_c", "fpga_die")
	READ_TEMP("temp_faceplate_c", "faceplate")
	READ_TEMP("temp_bottom_c", "bottom")
#undef READ_TEMP
	if (!got)
		printf("(sysfs thermal attributes not present — older module?)\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* decode                                                              */
static int cmd_decode(int argc, char **argv)
{
	const char *in_path = NULL, *out_path = NULL;
	uint32_t poll_ms = 30000;
	int nostop = 0, fd, in_fd, out_fd, ret;
	unsigned char *inmap, *outmap, *buf;
	struct ioc_status st;
	struct ioc_submit s;
	uint32_t sz = 0, in_size, out_len = 0, fw = 0, fh = 0;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--poll") && i + 1 < argc)
			poll_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--nostop"))
			nostop = 1;
		else if (!in_path)
			in_path = argv[i];
		else if (!out_path)
			out_path = argv[i];
	}
	if (!in_path) {
		fprintf(stderr, "usage: abctl decode <frame> [out.yuv] [--poll N]\n");
		return 1;
	}

	fd = open(dev_path, O_RDWR);
	if (fd < 0)
		die("open /dev/afterburner0");

	ret = ioctl(fd, IOC_CARD_STATUS, &st);
	if (ret)
		die("cardStatus");
	printf("[status] fw=%u.%u ready=%u dec_enable=0x%x engine_ctrl=0x%x\n",
	       (st.fw_version >> 16) & 0xFFFF, st.fw_version & 0xFFFF,
	       st.ready, st.dec_enable, st.engine_ctrl);
	if (!st.ready) {
		printf("[start] decoder not ready — issuing IOC_START ...\n");
		ret = ioctl(fd, IOC_START, NULL);
		if (ret)
			die("startFrame");
		ioctl(fd, IOC_CARD_STATUS, &st);
		printf("[start] ready=%u dec_enable=0x%x\n", st.ready, st.dec_enable);
		if (!st.ready) {
			fprintf(stderr, "decoder not ready after START — aborting\n");
			return 1;
		}
	}

	in_fd = open(in_path, O_RDONLY);
	if (in_fd < 0)
		die("open frame");
	buf = malloc(512u << 20); /* sanity cap; ProRes frames are small */
	if (!buf) { fprintf(stderr, "oom\n"); return 1; }
	while (sz < (uint32_t)(512u << 20)) {
		ssize_t r = read(in_fd, buf + sz, (512u << 20) - sz);

		if (r <= 0)
			break;
		sz += (uint32_t)r;
	}
	close(in_fd);
	if (!sz) { fprintf(stderr, "empty frame file\n"); return 1; }
	in_size = (sz + 4095) & ~4095u; /* page-align the bind size */

	/* parse W/H from the ProRes frame header (u16BE at +16/+18 of the box)
	 * and size output for the firmware's NATURAL padded layout (§35.2):
	 * interleaved [U,Y0,V,Y1], stride = ceil(W*2/64)*64, rows = ceil(H/16)*16 */
	if (sz >= 32) {
		fw = (uint16_t)((buf[16] << 8) | buf[17]);
		fh = (uint16_t)((buf[18] << 8) | buf[19]);
	}
	if (fw && fh) {
		uint32_t stride = ((fw * 2u + 63u) / 64u) * 64u;
		uint32_t rows = ((fh + 15u) / 16u) * 16u;

		out_len = stride * rows;
		printf("[frame] %s: %u bytes, header %ux%u — out_len=%u (padded)\n",
		       in_path, sz, fw, fh, out_len);
	} else {
		fprintf(stderr, "cannot parse W/H from frame header — pass a raw ProRes frame\n");
		return 1;
	}

	struct ioc_bind b = { in_size, out_len };
	ret = ioctl(fd, IOC_BIND_FRAME, &b);
	if (ret)
		die("bindFrame");

	inmap = mmap(NULL, in_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	outmap = mmap(NULL, out_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)1 * 4096);
	if (inmap == MAP_FAILED || outmap == MAP_FAILED)
		die("mmap");

	memcpy(inmap, buf, sz);
	free(buf);
	memset(outmap, 0x5A, out_len); /* sentinel: untouched region = partial decode */

	s.in_off = 0;
	s.in_len = sz;
	s.out_len = out_len;
	s.poll_ms = poll_ms;

	printf("[submit] in_len=%u out_len=%u poll_ms=%u ...\n", s.in_len, s.out_len, poll_ms);
	ret = ioctl(fd, IOC_SUBMIT_FRAME, &s);
	if (ret) {
		fprintf(stderr, "[FAIL] submit returned %d (%s)\n", ret, strerror(-ret));
		return 1;
	}
	printf("[submit] OK status=0x%08x data_word=0x%08x\n", s.status, s.data_word);

	if (out_path) {
		out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out_fd < 0)
			die("open output");
		uint32_t done = 0;

		while (done < out_len) {
			ssize_t w = write(out_fd, outmap + done, out_len - done);

			if (w <= 0)
				break;
			done += (uint32_t)w;
		}
		close(out_fd);
		printf("[dump] %s: %u bytes\n", out_path, done);
	} else {
		uint32_t untouched = 0;

		for (uint32_t i = 0; i < out_len; i++)
			if (outmap[i] == 0x5A)
				untouched++;
		printf("[check] %u/%u output bytes still sentinel (partial decode if >0)\n",
		       untouched, out_len);
	}

	if (!nostop) {
		ret = ioctl(fd, IOC_STOP, NULL);
		if (ret)
			die("stop");
		ioctl(fd, IOC_CARD_STATUS, &st);
		printf("[post-stop] ready=%u dec_enable=0x%x\n", st.ready, st.dec_enable);
	} else {
		printf("[skip] decoder left enabled (--nostop)\n");
	}

	ioctl(fd, IOC_UNBIND_ALL, NULL);
	close(fd);
	return 0;
}

/* ------------------------------------------------------------------ */
static int cmd_start(void)
{
	int fd = open(dev_path, O_RDWR), ret;
	struct ioc_status st;

	if (fd < 0)
		die("open /dev/afterburner0");
	ret = ioctl(fd, IOC_START, NULL);
	if (ret)
		die("startFrame");
	ioctl(fd, IOC_CARD_STATUS, &st);
	close(fd);
	printf("[start] ready=%u dec_enable=0x%x\n", st.ready, st.dec_enable);
	return 0;
}

static int cmd_stop(void)
{
	int fd = open(dev_path, O_RDWR), ret;
	struct ioc_status st;

	if (fd < 0)
		die("open /dev/afterburner0");
	ret = ioctl(fd, IOC_STOP, NULL);
	if (ret)
		die("stopFrame");
	ioctl(fd, IOC_CARD_STATUS, &st);
	close(fd);
	printf("[stop] ready=%u dec_enable=0x%x\n", st.ready, st.dec_enable);
	return 0;
}

static int cmd_unbind(void)
{
	int fd = open(dev_path, O_RDWR), ret;

	if (fd < 0)
		die("open /dev/afterburner0");
	ret = ioctl(fd, IOC_UNBIND_ALL, NULL);
	close(fd);
	if (ret)
		die("unbindAll");
	printf("[unbind] OK\n");
	return 0;
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: abctl {status|decode|start|stop|unbind} ...\n");
		return 1;
	}
	if (!strcmp(argv[1], "status"))
		return cmd_status();
	if (!strcmp(argv[1], "decode"))
		return cmd_decode(argc - 2, argv + 2);
	if (!strcmp(argv[1], "start"))
		return cmd_start();
	if (!strcmp(argv[1], "stop"))
		return cmd_stop();
	if (!strcmp(argv[1], "unbind"))
		return cmd_unbind();
	fprintf(stderr, "unknown subcommand: %s\n", argv[1]);
	return 1;
}
