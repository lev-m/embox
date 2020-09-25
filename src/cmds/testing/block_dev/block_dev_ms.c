
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

#include <util/pretty_print.h>
#include <drivers/block_dev.h>

static long long measure(struct block_dev* dev, size_t blkcnt, size_t block_size) {
	long long ret;

	char buffer[block_size];
	blkno_t all_blkcnt = dev->size / block_size;
	size_t original_size = dev->block_size;
	dev->block_size = block_size;

	struct timespec ts1, ts2;
	clock_gettime(CLOCK_REALTIME, &ts1);
	{
		for (size_t blkno = 0; blkno < blkcnt; blkno++) {
			if (dev->block_size != dev->driver->read(dev, buffer, dev->block_size, blkno % all_clkcnt)) {
				ret = -1;
				goto finalize;
			}
		}
	}
	clock_gettime(CLOCK_REALTIME, &ts2);
	ret = (ts2.tv_sec - ts1.tv_sec) * 1000 + (ts2.tv_nsec - ts1.tv_nsec) / 1000000;

finalize:
	dev->block_size = original_size;
	return ret;
}

static void dump_buf(char *buf, size_t cnt, char *fmt) {
	char msg[256];
	size_t step = pretty_print_row_len();

	printf("============================\n");
	printf("%s\n", fmt);
	printf("============================\n");
	while (cnt) {
		pretty_print_row(buf, cnt, msg);

		printf("%s\n", msg);

		if (cnt < step) {
			cnt = 0;
		} else {
			cnt -= step;
		}

		buf += step;
	}
	printf("============================\n");
}

int main(int argc, char **argv) {
	int d = open("/dev/xvda", O_RDWR);
	if (d < 0) {
		printf("err\n");
		return -1;
	}

	char buffer[512];

	//struct block_dev* bdev = block_dev_find("xvda");
	//bdev->driver->read(bdev, buffer, 512, 0);

	printf("%d\n", read(d, buffer, 512));
	dump_buf(buffer, sizeof buffer, "Read buffer");

	close(d);

	return 0;
}
