/* iotrace.c — LD_PRELOAD shim logging every ioctl() with timestamp + args.
 * Build: gcc -shared -fPIC -O2 -o iotrace.so iotrace.c -ldl
 * Use:   LD_PRELOAD=./iotrace.so ./prog ...  (log -> stderr or $IOTRACE_LOG) */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static FILE *logf = NULL;
static uint64_t t0_ns = 0;

__attribute__((constructor)) static void init(void)
{
	const char *p = getenv("IOTRACE_LOG");

	logf = p ? fopen(p, "w") : stderr;
	if (!logf) logf = stderr;
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	t0_ns = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static uint64_t now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec - t0_ns;
}

int ioctl(int fd, unsigned long req, ...)
{
	static int (*real)(int, unsigned long, ...) = NULL;
	va_list ap;
	void *arg;
	int rc;
	uint64_t t;

	if (!real) real = dlsym(RTLD_NEXT, "ioctl");
	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	t = now();
	rc = real(fd, req, arg);

	/* 'A' ioctls only — skip terminal noise */
	if (((req >> 8) & 0xff) == 'A') {
		fprintf(logf, "%12.3f fd=%d req=0x%lx rc=%d", t / 1e9, fd, req, rc);
		switch (req & 0xff) {
		case 0: /* SUBMIT_FRAME */
			if (arg) { const uint32_t *p = arg;

				fprintf(logf, " submit{in_len=%u out_len=%u poll_ms=%u} -> status=0x%x",
					p[1], p[2], p[4], rc ? 0 : *(uint32_t *)arg); }
			break;
		case 9: /* SUBMIT_ASYNC */
			if (arg) { const uint32_t *p = arg;

				fprintf(logf, " async{in_off=%u in_len=%u out_len=%u}", p[0], p[1], p[2]); }
			break;
		case 10: /* WAIT_COMPL */
			if (arg) { const uint32_t *p = arg;

				fprintf(logf, " wait -> status=0x%x data=0x%x", p[0], p[1]); }
			break;
		case 2: /* BIND_FRAME */
			if (arg) { const uint32_t *p = arg;

				fprintf(logf, " bind{in=%u out=%u}", p[0], p[1]); }
			break;
		default:
			if (arg && (req & 0x3000) == _IOC_READ) { const uint32_t *p = arg;

				fprintf(logf, " r{");
				for (int i = 0; i < _IOC_SIZE(req) / 4 && i < 8; i++)
					fprintf(logf, "%s0x%x", i ? "," : "", p[i]);
				fprintf(logf, "}"); }
			break;
		}
		fprintf(logf, "\n");
		fflush(logf);
	}
	return rc;
}
