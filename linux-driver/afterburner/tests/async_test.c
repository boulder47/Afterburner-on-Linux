/* async_test.c — Phase B (plan2) validation: SUBMIT_ASYNC / WAIT_COMPLETION
 * burst loop, space-pool accounting, getPercent duty cycle, and the depth-1
 * -EBUSY guard.  Usage:
 *   async_test <frame.bin> [out.yuv] [N]     (default N=4)
 *
 * Per frame: SUBMIT_ASYNC returns immediately; we sample GET_PERCENT while
 * the frame is in flight (getPercent takes only time_lock, so it works
 * concurrently), then WAIT_COMPLETION blocks until the decode completion +
 * 1 s c2h settle.  The stale send-channel event left at BAR1+0x2044 is
 * drained kext-exact by ab_drain() inside the next submit (verified in
 * dmesg: "pre-submit drain" lines). */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef uint32_t __u32;
typedef uint64_t __u64;

struct ioc_query  { __u32 status, data_word; };
struct ioc_bind   { __u32 in_size, out_size; };
struct ioc_status { __u32 fw_version, ready, dec_enable, engine_ctrl; };
struct ioc_space  { __u32 in_flight, max_in_flight; __u64 used_bytes, cap_bytes; };
struct ioc_async  { __u32 in_off, in_len, out_len; };

#define IOC_BIND_FRAME   _IOWR('A', 2, struct ioc_bind)
#define IOC_CARD_STATUS  _IOR ('A', 3, struct ioc_status)
#define IOC_CHECK_SPACE  _IOR ('A', 4, struct ioc_space)
#define IOC_UNBIND_ALL   _IO  ('A', 5)
#define IOC_GET_PERCENT  _IOR ('A', 6, __u32)
#define IOC_SUBMIT_ASYNC _IOWR('A', 9, struct ioc_async)
#define IOC_WAIT_COMPL   _IOR ('A',10, struct ioc_query)

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	const char *in_path = argv[1];
	const char *out_path = argc > 2 ? argv[2] : NULL;
	int nframes = argc > 3 ? atoi(argv[3]) : 4;
	int i_done = 0;
	unsigned char *frame, *outmap, *inmap;
	struct ioc_space sp;
	struct ioc_status st;
	uint16_t fw, fh;
	int stride, rows, out_len, fd, rc = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: async_test <frame.bin> [out.yuv] [N]\n");
		return 1;
	}

	FILE *f = fopen(in_path, "rb");

	if (!f) {
		perror("open frame");
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	frame = malloc((size_t)fsz);
	if (fread(frame, 1, (size_t)fsz, f) != (size_t)fsz) {
		fprintf(stderr, "short read\n");
		return 1;
	}
	fclose(f);

	/* ProRes frame header (abctl parity): u16BE w@+16 h@+18 of the box;
	 * bind size must be page-aligned (ab_mmap rejects non-page sizes). */
	fw = (uint16_t)((frame[16] << 8) | frame[17]);
	fh = (uint16_t)((frame[18] << 8) | frame[19]);
	stride = ((fw * 2 + 63) / 64) * 64;
	rows = ((fh + 15) / 16) * 16;
	out_len = stride * rows;
	fsz = (fsz + 4095) & ~4095L; /* page-align the bind size */
	frame = realloc(frame, fsz);
	if (!frame) { perror("realloc"); return 1; }

	fd = open("/dev/afterburner0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/afterburner0");
		return 1;
	}

	if (ioctl(fd, IOC_CARD_STATUS, &st)) {
		perror("cardStatus");
		return 1;
	}
	printf("[status] fw=%u.%u ready=%u dec_enable=0x%x engine_ctrl=0x%x\n",
	       (st.fw_version >> 16) & 0xFFFF, st.fw_version & 0xFFFF,
	       st.ready, st.dec_enable, st.engine_ctrl);

	if (ioctl(fd, IOC_CHECK_SPACE, &sp)) {
		perror("checkSpace pre-bind");
		return 1;
	}
	printf("[space] pre-bind : in_flight=%u/%u used=%.2f MiB cap=%.0f MiB\n",
	       sp.in_flight, sp.max_in_flight,
	       (double)sp.used_bytes / 1048576.0,
	       (double)sp.cap_bytes / 1048576.0);

	__u32 pct = 999;

	if (ioctl(fd, IOC_GET_PERCENT, &pct)) {
		perror("getPercent baseline");
		return 1;
	}
	printf("[percent] baseline (idle window): %u%%\n", pct);

	struct ioc_bind b = { (uint32_t)fsz, (uint32_t)out_len };

	if (ioctl(fd, IOC_BIND_FRAME, &b)) {
		perror("bindFrame");
		return 1;
	}
	if (ioctl(fd, IOC_CHECK_SPACE, &sp)) {
		perror("checkSpace post-bind");
		return 1;
	}
	printf("[space] post-bind: in_flight=%u/%u used=%.2f MiB cap=%.0f MiB "
	       "(expect used = in+out = %.2f MiB)\n",
	       sp.in_flight, sp.max_in_flight,
	       (double)sp.used_bytes / 1048576.0,
	       (double)sp.cap_bytes / 1048576.0,
	       ((double)fsz + out_len) / 1048576.0);

	inmap = mmap(NULL, (size_t)fsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	outmap = mmap(NULL, (size_t)out_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		      fd, (off_t)1 * 4096);
	if (inmap == MAP_FAILED || outmap == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memcpy(inmap, frame, (size_t)fsz);
	memset(outmap, 0x5A, (size_t)out_len); /* sentinel: untouched = partial */

	double t_start = now_ms();

	for (int i = 0; i < nframes; i++) {
		struct ioc_async a = { 0, (uint32_t)fsz, (uint32_t)out_len };
		struct ioc_query q;
		double t_sub, t_smp, t_done;

		if (ioctl(fd, IOC_SUBMIT_ASYNC, &a)) {
			perror("submitAsync");
			rc = 1;
			break;
		}
		t_sub = now_ms();

		/* depth-1 guard: a second ring while unreaped must be -EBUSY */
		if (i == 0) {
			int e = ioctl(fd, IOC_SUBMIT_ASYNC, &a);

			if (e && errno != EBUSY) {
				fprintf(stderr, "double-submit: expected EBUSY, got %s\n",
					strerror(errno));
				rc = 1;
				break;
			}
			printf("[guard] double submitAsync -> %s (expect Resource busy)\n",
			       e ? strerror(errno) : "no error?!");
		}

		if (ioctl(fd, IOC_GET_PERCENT, &pct)) {
			perror("getPercent in-flight");
			rc = 1;
			break;
		}
		t_smp = now_ms();

		if (ioctl(fd, IOC_WAIT_COMPL, &q)) {
			fprintf(stderr, "frame %d: waitCompl failed: %s\n", i,
				strerror(errno));
			rc = 1;
			break;
		}
		t_done = now_ms();

		printf("[frame %d] in-flight sample %.0f ms (pct=%u%%)  "
		       "wait->done %.0f ms  status=0x%08x data=0x%x\n",
		       i, t_smp - t_sub, pct, t_done - t_smp, q.status,
		       q.data_word);
		if (!(q.status & 1) || q.data_word != 6) {
			fprintf(stderr, "frame %d: unexpected completion "
				"(want valid-bit set + data=6)\n", i);
			rc = 1;
			break;
		}
		i_done++;
	}
	double t_end = now_ms();

	if (ioctl(fd, IOC_GET_PERCENT, &pct)) {
		perror("getPercent post-burst");
		return 1;
	}
	printf("[percent] post-burst: %u%%   (%d/%d frames in %.0f ms wall)\n",
	       pct, i_done, nframes, t_end - t_start);

	if (out_path && rc == 0) {
		FILE *o = fopen(out_path, "wb");

		if (!o) {
			perror("open out");
			return 1;
		}
		if (fwrite(outmap, 1, (size_t)out_len, o) != (size_t)out_len)
			fprintf(stderr, "short write to %s\n", out_path);
		fclose(o);
		printf("[dump] %s: %d bytes (last frame)\n", out_path, out_len);
	}

	if (ioctl(fd, IOC_UNBIND_ALL)) {
		perror("unbindAll");
		return 1;
	}
	if (ioctl(fd, IOC_CHECK_SPACE, &sp)) {
		perror("checkSpace post-unbind");
		return 1;
	}
	printf("[space] post-unbind: in_flight=%u/%u used=%.2f MiB cap=%.0f MiB "
	       "(expect zeroed — kext unbindAll parity)\n",
	       sp.in_flight, sp.max_in_flight,
	       (double)sp.used_bytes / 1048576.0,
	       (double)sp.cap_bytes / 1048576.0);

	close(fd);
	return rc;
}
