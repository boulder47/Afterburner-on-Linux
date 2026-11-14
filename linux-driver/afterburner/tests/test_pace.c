/* test_pace.c — wedge-trigger isolation harness.
 *
 * Replicates abplayer's per-frame sequence with three toggleable factors:
 *   -r  re-stage input (memcpy pkt -> inmap) before every ring [default ON]
 *       (pass --no-restage to stage once, async_test style)
 *   -p N pace sleep of N ms after each frame [default 0 = tight loop]
 *   -o  read the output buffer (4 MB memcpy to scratch) after each wait
 *       [default OFF; abplayer does this via SDL_UpdateTexture]
 *
 * Usage: test_pace <frame.bin> [N] [--no-restage] [-p ms] [-o]
 * Exits 0 if all N frames complete, 1 on wedge (prints which frame). */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef uint32_t __u32;
typedef uint64_t __u64;

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

struct ioc_query  { __u32 status, data_word; };
struct ioc_bind   { __u32 in_size, out_size; };
struct ioc_status { __u32 fw_version, ready, dec_enable, engine_ctrl; };
struct ioc_async  { __u32 in_off, in_len, out_len; };

#define IOC_BIND_FRAME   _IOWR('A', 2, struct ioc_bind)
#define IOC_CARD_STATUS  _IOR ('A', 3, struct ioc_status)
#define IOC_UNBIND_ALL   _IO  ('A', 5)
#define IOC_SUBMIT_ASYNC _IOWR('A', 9, struct ioc_async)
#define IOC_WAIT_COMPL   _IOR ('A',10, struct ioc_query)

int main(int argc, char **argv)
{
	const char *in_path = argv[1];
	int nframes = argc > 2 ? atoi(argv[2]) : 30;
	int restage = 1, pace_ms = 0, outread = 0, bind_size = 0;
	const char *mov_path = NULL;
	uint64_t mov_off = 0;

	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "--no-restage")) restage = 0;
		else if (!strcmp(argv[i], "-p") && i + 1 < argc) pace_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-o")) outread = 1;
		else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_size = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mov") && i + 2 < argc) { mov_path = argv[++i]; mov_off = strtoull(argv[++i], NULL, 0); }
	}

	FILE *f = fopen(in_path, "rb");
	if (!f) { perror("open frame"); return 2; }
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *frame = malloc((size_t)fsz);
	if (fread(frame, 1, (size_t)fsz, f) != (size_t)fsz) { fprintf(stderr, "short read\n"); return 2; }
	fclose(f);

	/* --mov: source packets from a MAP_PRIVATE mmap of the container file,
	 * exactly like abplayer does (fdata + offsets[i]). */
	unsigned char *fdata = NULL;
	if (mov_path) {
		int mfd = open(mov_path, O_RDONLY);

		if (mfd < 0) { perror("open mov"); return 2; }
		struct stat stb;

		if (fstat(mfd, &stb)) { perror("fstat mov"); return 2; }
		fdata = mmap(NULL, (size_t)stb.st_size, PROT_READ, MAP_PRIVATE, mfd, 0);
		close(mfd);
		if (fdata == MAP_FAILED) { perror("mmap mov"); return 2; }
	}

	uint16_t fw = (uint16_t)((frame[16] << 8) | frame[17]);
	uint16_t fh = (uint16_t)((frame[18] << 8) | frame[19]);
	int stride = ((fw * 2 + 63) / 64) * 64;
	int rows = ((fh + 15) / 16) * 16;
	int out_len = stride * rows;
	long psz = (fsz + 4095) & ~4095L;

	int fd = open("/dev/afterburner0", O_RDWR);
	if (fd < 0) { perror("open /dev/afterburner0"); return 2; }

	struct ioc_status st;
	if (ioctl(fd, IOC_CARD_STATUS, &st)) { perror("cardStatus"); return 2; }
	printf("[cfg] fw=%u.%u ready=%u dec=0x%x | N=%d restage=%d pace=%dms outread=%d\n",
	       (st.fw_version >> 16) & 0xFFFF, st.fw_version & 0xFFFF,
	       st.ready, st.dec_enable, nframes, restage, pace_ms, outread);

	struct ioc_bind b = { (uint32_t)(bind_size ? bind_size : psz), (uint32_t)out_len };
	if (ioctl(fd, IOC_BIND_FRAME, &b)) { perror("bindFrame"); return 2; }

	unsigned char *inmap = mmap(NULL, (size_t)b.in_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	unsigned char *outmap = mmap(NULL, (size_t)out_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)4096);
	if (inmap == MAP_FAILED || outmap == MAP_FAILED) { perror("mmap"); return 2; }

	unsigned char *scratch = malloc((size_t)out_len); /* sink for -o reads */
	const unsigned char *src = fdata ? fdata + mov_off : frame;
	memcpy(inmap, src, (size_t)fsz);

	int done = 0;
	int n_status2 = 0; /* completions reaped at status=0x2 (valid bit not yet set) */
	uint64_t period_ns = pace_ms ? (uint64_t)pace_ms * 1000000ull : 0;
	for (int i = 0; i < nframes; i++) {
		uint64_t t_frame_start = now_ns();

		if (restage || i == 0)
			memcpy(inmap, src, (size_t)fsz);

		struct ioc_async a = { 0, (uint32_t)fsz, (uint32_t)out_len };
		if (ioctl(fd, IOC_SUBMIT_ASYNC, &a)) { perror("submitAsync"); break; }

		struct ioc_query q;
		for (int attempt = 0; ; attempt++) {
			if (!ioctl(fd, IOC_WAIT_COMPL, &q)) break;
			if (attempt >= 3) {
				printf("[WEDGE] frame %d: waitCompl failed after recovery ladder (%s)\n",
				       i, strerror(errno));
				goto out;
			}
			/* minimal recovery: unbind + rebind + restage */
			ioctl(fd, IOC_UNBIND_ALL);
			munmap(inmap, (size_t)b.in_size);
			munmap(outmap, (size_t)out_len);
			if (ioctl(fd, 0x4108 /* AB_IOC_START = _IO('A',8) */, NULL)) { /* best effort */ }
			usleep(50 * 1000);
			if (ioctl(fd, IOC_BIND_FRAME, &b)) { perror("rebind"); goto out; }
			inmap = mmap(NULL, (size_t)b.in_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			outmap = mmap(NULL, (size_t)out_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)4096);
			memcpy(inmap, frame, (size_t)fsz);
			if (ioctl(fd, IOC_SUBMIT_ASYNC, &a)) { perror("resubmit"); goto out; }
		}
		if (!(q.status & 2) || q.data_word != 6) {
			printf("[WEDGE] frame %d: bad completion status=0x%08x data=0x%x\n",
			       i, q.status, q.data_word);
			goto out;
		}
		if (q.status == 2) n_status2++;
		done++;

		if (outread)
			memcpy(scratch, outmap, (size_t)out_len);
		if (period_ns) { /* strict periodic pacing: ring-to-ring = period */
			uint64_t elapsed = now_ns() - t_frame_start;

			if (elapsed < period_ns) {
				struct timespec rq = {
					.tv_sec  = (period_ns - elapsed) / 1000000000ull,
					.tv_nsec = (long)((period_ns - elapsed) % 1000000000ull),
				};

				while (nanosleep(&rq, &rq) == -1 && errno == EINTR);
			}
		}
	}
	printf("[OK] %d/%d frames (status=0x2 reaps: %d)\n", done, nframes, n_status2);

out:
	ioctl(fd, IOC_UNBIND_ALL);
	close(fd);
	return done == nframes ? 0 : 1;
}
