#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#define AB_IOC_GET_PERCENT _IOR('A', 6, uint32_t)
int main(void) {
	int fd = open("/dev/afterburner0", O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	unsigned int p = 0xDEAD;
	int ret = ioctl(fd, AB_IOC_GET_PERCENT, &p);
	printf("ret=%d percent=0x%08x (%u)\n", ret, p, p);
	return ret ? 1 : 0;
}
