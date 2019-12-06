/**
 * @file
 * @brief Implementation of FAT driver for DVFS
 *
 * @date   11 Apr 2015
 * @author Denis Deryugin <deryugin.denis@gmail.com>
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <limits.h>

#include <drivers/device.h>

#include <fs/dvfs.h>
#include <util/math.h>
#include <util/log.h>

#include "fat.h"

#define DEFAULT_FAT_VERSION OPTION_GET(NUMBER, default_fat_version)

/**
 * @brief Set appropriate flags and i_data for given inode
 *
 * @param inode Inode to be filled
 * @param di FAT directory entry related to file
 *
 * @return Negative error code or zero if succeed
 */
static int fat_fill_inode(struct inode *inode, struct fat_dirent *de, struct dirinfo *di) {
	struct fat_file_info *fi;
	struct fat_fs_info *fsi;
	struct dirinfo *new_di;
	struct volinfo *vi;
	struct super_block *sb;
	int res, tmp_sector, tmp_entry, tmp_cluster;

	assert(de);
	assert(inode);
	assert(di);

	sb = inode->i_sb;
	assert(sb);

	fsi = sb->sb_data;
	assert(fsi);

	vi = &fsi->vi;
	assert(vi);

	/* Need to save some dirinfo data because this
	 * stuff may change while we traverse to the end
	 * of long name entry */
	tmp_sector = di->currentsector;
	tmp_entry = di->currententry;
	tmp_cluster = di->currentcluster;

	while (de->attr == ATTR_LONG_NAME) {
		res = fat_get_next(di, de);

		if (res != DFS_OK && res != DFS_ALLOCNEW) {
			return -EINVAL;
		}
	}

	if (de->attr & ATTR_DIRECTORY){
		if (NULL == (new_di = fat_dirinfo_alloc()))
			goto err_out;

		memset(new_di, 0, sizeof(struct dirinfo));
		new_di->p_scratch = fat_sector_buff;
		inode->flags |= S_IFDIR;

		new_di->currentcluster = (uint32_t) de->startclus_l_l |
		  ((uint32_t) de->startclus_l_h) << 8 |
		  ((uint32_t) de->startclus_h_l) << 16 |
		  ((uint32_t) de->startclus_h_h) << 24;

		fi = &new_di->fi;
	} else {
		if (NULL == (fi = fat_file_alloc())) {
			goto err_out;
		}
	}

	inode->i_data = fi;

	*fi = (struct fat_file_info) {
		.fsi = fsi,
		.volinfo = vi,
	};

	fi->dirsector = tmp_sector + fat_sec_by_clus(fsi, tmp_cluster);
	fi->diroffset = tmp_entry - 1;
	fi->cluster = (uint32_t) de->startclus_l_l |
	  ((uint32_t) de->startclus_l_h) << 8 |
	  ((uint32_t) de->startclus_h_l) << 16 |
	  ((uint32_t) de->startclus_h_h) << 24;
	fi->firstcluster = fi->cluster;
	fi->filelen = (uint32_t) de->filesize_0 |
			      ((uint32_t) de->filesize_1) << 8 |
			      ((uint32_t) de->filesize_2) << 16 |
			      ((uint32_t) de->filesize_3) << 24;
	fi->fdi = di;

	inode->length    = fi->filelen;
	inode->start_pos = fi->firstcluster * fi->volinfo->secperclus * fi->volinfo->bytepersec;
	if (de->attr & ATTR_READ_ONLY)
		inode->flags |= S_IRALL;
	else
		inode->flags |= S_IRWXA;
	return 0;
err_out:
	return -1;
}

/* @brief Figure out if node at specific path exists or not
 * @note  Assume dir is root
 * @note IMPORTANT: this functions should not be calls in the middle of iterate,
 * as it wipes dirinfo content
 *
 * @param name Full path to the extected node
 * @param dir  Not used now
 *
 * @return Pointer of inode or NULL if not found
 */
static struct inode *fat_ilookup(char const *name, struct dentry const *dir) {
	struct dirinfo *di;
	struct fat_dirent de;
	struct super_block *sb;
	uint8_t tmp_ent;
	uint8_t tmp_sec;
	uint32_t tmp_clus;
	struct inode *node;
	char tmppath[PATH_MAX];
	int found = 0;

	assert(name);
	assert(dir->d_inode);
	assert(FILE_TYPE(dir->d_inode->flags, S_IFDIR));

	sb = dir->d_sb;
	di = dir->d_inode->i_data;

	assert(di);

	tmp_ent = di->currententry;
	tmp_sec = di->currentsector;
	tmp_clus = di->currentcluster;
	fat_reset_dir(di);

	if (read_dir_buf(di)) {
		goto err_out;
	}

	while (!fat_get_next_long(di, &de, tmppath)) {
		if (!strncmp(tmppath, name, sizeof(tmppath))) {
			found = 1;
			break;
		}
	}

	if (!found)
		goto err_out;

	if (NULL == (node = dvfs_alloc_inode(sb)))
		goto err_out;

	if (fat_fill_inode(node, &de, di))
		goto err_out;

	goto succ_out;
err_out:
	node = NULL;
succ_out:
	di->currentcluster = tmp_clus;
	di->currententry = tmp_ent;
	di->currentsector = tmp_sec;
	return node;
}

/* @brief Create new file or directory
 * @param i_new Inode to be filled
 * @param i_dir Inode realted to the parent
 * @param mode  Used to figure out file type
 *
 * @return Negative error code
 */
static int fat_create(struct inode *i_new, struct inode *i_dir, int mode) {
	struct fat_file_info *fi;
	struct fat_fs_info *fsi;
	struct dirinfo *di, *new_di;
	char *name;

	assert(i_new);
	assert(i_dir);

	/* TODO check file exists */
	assert(i_dir->i_sb);
	fsi = i_dir->i_sb->sb_data;
	di = i_dir->i_data;

	fat_reset_dir(di);
	read_dir_buf(di);

	assert(i_new->i_dentry);
	name = i_new->i_dentry->name;
	assert(name);

	if (FILE_TYPE(mode, S_IFDIR)) {
		if (!(new_di = fat_dirinfo_alloc())) {
			return -ENOMEM;
		}
		new_di->p_scratch = fat_sector_buff;
		fi = &new_di->fi;
	} else {
		if (!(fi = fat_file_alloc())) {
			return -ENOMEM;
		}
	}
	fi->fsi     = fsi;
	fi->volinfo = &fsi->vi;
	fi->fdi     = di;

	i_new->i_data = fi;

	return fat_create_file(fi, di, name, mode);
}

static int fat_close(struct file_desc *desc) {
	return 0;
}

static size_t fat_read(struct file_desc *desc, void *buf, size_t size) {
	uint32_t res;
	struct fat_file_info *fi;

	fi = file_get_inode_data(desc);
	fi->pointer = file_get_pos(desc);

	fat_read_file(fi, fat_sector_buff, buf, &res, min(size, fi->filelen - desc->pos));

	return res;
}

static size_t fat_write(struct file_desc *desc, void *buf, size_t size) {
	uint32_t res;
	struct fat_file_info *fi;

	fi = file_get_inode_data(desc);
	fi->pointer = file_get_pos(desc);

	fi->mode = O_RDWR; /* XXX */

	fat_write_file(fi, fat_sector_buff, buf, &res, size, &desc->f_inode->length);
	fi->filelen = desc->f_inode->length;
	return res;
}

/* @brief Get next inode in directory
 * @param inode   Structure to be filled
 * @param parent  Inode of parent directory
 * @param dir_ctx Directory context
 *
 * @return Error code
 */
static int fat_iterate(struct inode *next, struct inode *parent, struct dir_ctx *ctx) {
	struct dirinfo *dirinfo;
	struct fat_dirent de;
	char path[PATH_MAX];
	int res;

	if (!parent)
		strcpy(path, ROOT_DIR);

	assert(parent->i_sb);

	dirinfo = parent->i_data;
	dirinfo->currententry = (uintptr_t) ctx->fs_ctx;

	if (dirinfo->currententry == 0) {
		/* Need to get directory data from drive */
		fat_reset_dir(dirinfo);
	}

	read_dir_buf(dirinfo);

	while (((res = fat_get_next_long(dirinfo, &de, NULL)) ==  DFS_OK) || res == DFS_ALLOCNEW) {
		if (!memcmp(de.name, MSDOS_DOT, strlen(MSDOS_DOT)) ||
			!memcmp(de.name, MSDOS_DOTDOT, strlen(MSDOS_DOT))) {
			continue;
		}
		break;
	}

	switch (res) {
	case DFS_OK:
		fat_fill_inode(next, &de, dirinfo);
		ctx->fs_ctx = (void *) ((uintptr_t) dirinfo->currententry);
		return 0;
	case DFS_EOF:
		/* Fall through */
	default:
		return -1;
	}
}

static int fat_remove(struct inode *inode) {
	struct fat_file_info *fi;
	struct dirinfo *di;
	int res;

	if (FILE_TYPE(inode->flags, S_IFDIR)) {
		di = inode->i_data;
		res = fat_unlike_directory(&di->fi, NULL, (uint8_t*) fat_sector_buff);
	} else {
		fi = inode->i_data;
		res = fat_unlike_file(fi, NULL, (uint8_t*) fat_sector_buff);
	}
	return res;
}

static int fat_pathname(struct inode *inode, char *buf, int flags) {
	struct fat_file_info *fi;

	switch (flags) {
	case DVFS_NAME:
		fi = inode->i_data;

		if (DFS_OK != fat_read_filename(fi, fat_sector_buff, buf)) {
			return -1;
		}
		return 0;
	default:
		/* NIY */
		return -ENOSYS;
	}
}

static int fat_truncate(struct inode *inode, size_t len) {
	/* This is a stub, but files should be extended automatically
	 * with the common part of the driver on write */
	return 0;
}

/* Declaration of operations */
struct inode_operations fat_iops = {
	.create   = fat_create,
	.lookup   = fat_ilookup,
	.remove   = fat_remove,
	.iterate  = fat_iterate,
	.pathname = fat_pathname,
	.truncate = fat_truncate,
};

static struct file_operations fat_fops = {
	.close = fat_close,
	.write = fat_write,
	.read = fat_read,
};

static int fat_destroy_inode(struct inode *inode) {
	struct fat_file_info *fi;
	struct dirinfo *di;

	if (!inode->i_data)
		return 0;

	if (FILE_TYPE(inode->flags, S_IFDIR)) {
		di = inode->i_data;
		fat_dirinfo_free(di);
	} else {
		fi = inode->i_data;
		fat_file_free(fi);
	}

	return 0;
}

struct super_block_operations fat_sbops = {
	.open_idesc    = dvfs_file_open_idesc,
	.destroy_inode = fat_destroy_inode,
};

/* @brief Initializing fat super_block
 * @param sb  Structure to be initialized
 * @param dev Storage device
 *
 * @return Negative error code
 */
static int fat_fill_sb(struct super_block *sb, struct file_desc *bdev_file) {
	struct fat_fs_info *fsi;
	struct block_dev *dev;
	assert(sb);
	if (!bdev_file) {
		/* FAT always uses block device, so we can't fill superblock */
		return -ENOENT;
	}

	assert(bdev_file->f_inode);
	assert(bdev_file->f_inode->i_data);

	dev = ((struct dev_module *) bdev_file->f_inode->i_data)->dev_priv;
	assert(dev);

	fsi = fat_fs_alloc();
	*fsi = (struct fat_fs_info) {
		.bdev = dev,
	};
	sb->sb_data = fsi;
	sb->sb_iops = &fat_iops;
	sb->sb_fops = &fat_fops;
	sb->sb_ops  = &fat_sbops;

	if (fat_get_volinfo(dev, &fsi->vi, 0))
		goto err_out;

	return 0;

err_out:
	fat_fs_free(fsi);
	return -1;
}

/**
* @brief Initialize dirinfo for root FAT directory
* @note  Should be called just after mounting FAT FS
*
* @param super_block Superblock of just
*
* @return Negative error code
* @retval 0 Success
*/
static int fat_mount_end(struct super_block *sb) {
	struct dirinfo *di;
	struct fat_fs_info *fsi;

	uint8_t tmp[] = { '\0' };

	assert(sb);
	assert(sb->bdev);
	assert(sb->bdev->block_size <= FAT_MAX_SECTOR_SIZE);

	if (NULL == (di = fat_dirinfo_alloc())) {
		return -ENOMEM;
	}

	di->p_scratch = fat_sector_buff;

	fsi = sb->sb_data;
	assert(fsi);

	if (fat_open_dir(fsi, tmp, di)) {
		fat_dirinfo_free(di);
		return -1;
	}

	di->fi = (struct fat_file_info) {
		.fsi          = fsi,
		.volinfo      = &fsi->vi,
		.dirsector    = 0,
		.diroffset    = 0,
		.firstcluster = 0,
	};

	sb->root->d_inode->i_data = di;
	sb->root->d_inode->flags |= S_IFDIR;

	return 0;
}


/**
 * @brief Format given block device
 * @param dev Pointer to device
 * @note Should be block device
 *
 * @return Negative error code or 0 if succeed
 */
static int fat_format(void *dev, void *priv) {
	int fat_n = priv ? atoi((char*) priv) : 0;
	struct block_dev *bdev;

	assert(dev);

	if (!fat_n) {
		fat_n = DEFAULT_FAT_VERSION;
	}

	if (fat_n != 12 && fat_n != 16 && fat_n != 32) {
		log_error("Unsupported FAT version: FAT%d "
				"(FAT12/FAT16/FAT32 available)", fat_n);
		return -EINVAL;
	}

	bdev = dev_module_to_bdev(dev);
	fat_create_partition(bdev, fat_n);
	fat_root_dir_record(bdev);

	return 0;
}

/**
 * @brief Cleanup FS-specific stuff. No need to clean all files: VFS should
 * do it by itsekft
 *
 * @param sb Pointer to superblock
 *
 * @return Negative error code or 0 if succeed
 */
static int fat_clean_sb(struct super_block *sb) {
	struct fat_fs_info *fsi;
	struct dirinfo *di;

	assert(sb);
	assert(sb->root);
	assert(sb->root->d_inode);

	di = sb->root->d_inode->i_data;
	assert(di);
	fat_dirinfo_free(di);

	fsi = sb->sb_data;
	assert(fsi);
	fat_fs_free(fsi);

	return 0;
}

static const struct dumb_fs_driver dfs_fat_driver = {
	.name      = "vfat",
	.fill_sb   = fat_fill_sb,
	.mount_end = fat_mount_end,
	.format    = fat_format,
	.clean_sb  = fat_clean_sb,
};

ARRAY_SPREAD_DECLARE(const struct dumb_fs_driver *const, dumb_drv_tab);
ARRAY_SPREAD_ADD(dumb_drv_tab, &dfs_fat_driver);
