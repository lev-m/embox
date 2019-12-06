/**
 * @file
 *
 * @date 17 Mar 2015
 * @author Denis Deryugin
 */
#include <sys/types.h>

#include <fs/dvfs.h>
#include <fs/file_desc.h>

off_t file_get_pos(struct file_desc *file) {
	return file->pos;
}

off_t file_set_pos(struct file_desc *file, off_t off) {
	file->pos = off;
	return file->pos;
}

void *file_get_inode_data(struct file_desc *file) {
	return file->f_inode->i_data;
}
