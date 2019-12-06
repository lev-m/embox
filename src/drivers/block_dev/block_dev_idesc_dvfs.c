/**
 * @file
 * @brief DVFS-specific bdev handling
 * @author Denis Deryugin <deryugin.denis@gmail.com>
 * @version 0.1
 * @date 2015-10-01
 */
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <stddef.h>
#include <util/err.h>

#include <drivers/block_dev.h>
#include <drivers/device.h>
#include <fs/file_desc.h>

static void bdev_idesc_close(struct idesc *desc) {
}

static ssize_t bdev_idesc_read(struct idesc *desc, const struct iovec *iov, int cnt) {
	void *buf;
	size_t nbyte;
	struct file *file;
	struct block_dev *bdev;
	struct dev_module *devmod;
	size_t blk_no;
	int res;
	off_t pos;

	assert(iov);
	buf = iov->iov_base;
	assert(cnt == 1);
	nbyte = iov->iov_len;

	file = (struct file *) desc;

	pos = file_get_pos(file);

	devmod = file_get_inode_data(file);
	bdev = devmod->dev_priv;
	if (!bdev->parent_bdev) {
		/* It's not a partition */
		blk_no = pos / bdev->block_size;
	} else {
		/* It's a partition */
		blk_no = bdev->start_offset + (pos / bdev->block_size);
		bdev = bdev->parent_bdev;
	}

	assert(bdev->driver);
	assert(bdev->driver->read);
	res = bdev->driver->read(bdev, buf, nbyte, blk_no);
	if (res < 0) {
		return res;
	}
	file_set_pos(file, pos + res);

	return res;
}

static ssize_t bdev_idesc_write(struct idesc *desc, const struct iovec *iov, int cnt) {
	struct file *file;
	struct dev_module *devmod;
	struct block_dev *bdev;
	size_t blk_no;
	int res;
	off_t pos;

	assert(desc);
	assert(iov);
	assert(cnt == 1);

	file = (struct file *)desc;

	pos = file_get_pos(file);

	devmod = file_get_inode_data(file);
	bdev = devmod->dev_priv;
	if (!bdev->parent_bdev) {
		/* It's not a partition */
		blk_no = pos / bdev->block_size;
	} else {
		/* It's a partition */
		blk_no = bdev->start_offset + (pos / bdev->block_size);
		bdev = bdev->parent_bdev;
	}

	assert(bdev->driver);
	assert(bdev->driver->write);
	res = bdev->driver->write(bdev, (void *)iov->iov_base, iov->iov_len, blk_no);
	if (res < 0) {
		return res;
	}
	file_set_pos(file, pos + res);

	return res;
}

static int bdev_idesc_ioctl(struct idesc *idesc, int cmd, void *args) {
	struct dev_module *devmod;
	struct block_dev *bdev;
	struct file *file;

	assert(idesc);

	file = (struct file *) idesc;

	devmod = file_get_inode_data(file);
	bdev = devmod->dev_priv;

	switch (cmd) {
	case IOCTL_GETDEVSIZE:
		return bdev->size;
	case IOCTL_GETBLKSIZE:
		return bdev->block_size;
	default:
		if (bdev->parent_bdev) {
			bdev = bdev->parent_bdev;
		}
		assert(bdev->driver);
		if (NULL == bdev->driver->ioctl)
			return -ENOSYS;

		return bdev->driver->ioctl(bdev, cmd, args, 0);
	}
}

static int bdev_idesc_fstat(struct idesc *idesc, void *buff) {
	struct stat *sb;

	assert(buff);
	sb = buff;
	memset(sb, 0, sizeof(struct stat));
	sb->st_mode = S_IFBLK;

	return 0;
}

struct idesc_ops idesc_bdev_ops = {
	.close = bdev_idesc_close,
	.id_readv  = bdev_idesc_read,
	.id_writev = bdev_idesc_write,
	.ioctl = bdev_idesc_ioctl,
	.fstat = bdev_idesc_fstat
};
