// SPDX-License-Identifier: GPL-2.0
/* v5_setparam.c — set an afterburner module parameter (run as root via sudo;
 * the sysfs param files are 0644 and `tee` is not on this box's allowlist).
 * Usage: ./v5_setparam <name> <value>   e.g. ./v5_setparam ab_test_hang_ms 5000 */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	char path[256];
	FILE *f;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <param_name> <value>\n", argv[0]);
		return 1;
	}
	snprintf(path, sizeof(path), "/sys/module/afterburner/parameters/%s", argv[1]);
	f = fopen(path, "w");
	if (!f) { perror(path); return 1; }
	fprintf(f, "%s\n", argv[2]);
	fclose(f);
	printf("set %s=%s\n", argv[1], argv[2]);
	return 0;
}
