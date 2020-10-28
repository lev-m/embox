
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

struct result {
	long long blocks_time;
	long long size_time;
};

static int measure(struct result* res, struct block_dev* dev, size_t block_size, size_t read_blocks, size_t read_size) {
	int ret;

	char buffer[block_size];
	char b[block_size];
	memset(b, 1, block_size);
	blkno_t read_size_blocks = read_size / block_size;
	size_t original_size = dev->block_size;
	dev->block_size = block_size;

	struct timespec ts_begin, ts_cnt, ts_size;
	clock_gettime(CLOCK_REALTIME, &ts_begin);
	{
		blkno_t no = 0;

		for (; no < read_blocks; no++) {
			if (dev->block_size != dev->driver->write(dev, b, block_size, no)) {
				ret = -1;
				goto finalize;
			}

			if (dev->block_size != dev->driver->read(dev, buffer, block_size, no)) {
				ret = -1;
				goto finalize;
			}

			if (memcmp(b, buffer, block_size) != 0) {
				ret = -1;
				goto finalize;
			}
		}
		clock_gettime(CLOCK_REALTIME, &ts_cnt);

		for (; no < read_size_blocks; no++) {
			if (dev->block_size != dev->driver->read(dev, buffer, block_size, no)) {
				ret = -1;
				goto finalize;
			}
		}
		clock_gettime(CLOCK_REALTIME, &ts_size);
	}
	res->blocks_time = (ts_cnt.tv_sec - ts_begin.tv_sec) * 1000 + (ts_cnt.tv_nsec - ts_begin.tv_nsec) / 1000000;
	res->size_time = (ts_size.tv_sec - ts_begin.tv_sec) * 1000 + (ts_size.tv_nsec - ts_begin.tv_nsec) / 1000000;
	ret = 0;

finalize:
	dev->block_size = original_size;
	return ret;
}

int main(int argc, char **argv) {
	struct block_dev* bdev = block_dev_find(argv[1]);
	if (!bdev) {
		printf("Block device \"%s\" not found\n", argv[argc - 1]);
		return -EINVAL;
	}

	for (int i = 0; i < 10; i++) {
		for (size_t size = 1 << 15; size >= 1 << 9; size >>= 1) {
			struct result res;
			if (measure(&res, bdev, size, 1 << 5, 1 << 20) != 0) {
				printf("Error while reading");
				return -1;
			}

			printf("%d,%lld,%lld\n", size, res.blocks_time, res.size_time);
		}
	}

	return 0;
}
