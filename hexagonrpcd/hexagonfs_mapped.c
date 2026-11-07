/*
 * HexagonFS mapped file/directory operations
 *
 * Copyright (C) 2023 The Sensor Shell Contributors
 *
 * This file is part of sensh.
 *
 * Sensh is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "hexagonfs.h"

struct mapped_ctx {
	int fd;
	DIR *dir;
	bool writable;
};

static void mapped_close(void *fd_data)
{
	struct mapped_ctx *ctx = fd_data;

	if (ctx->dir != NULL)
		closedir(ctx->dir);
	else
		close(ctx->fd);

	free(ctx);
}

static int mapped_from_dirent(const void *dirent_data, bool dir, void **fd_data)
{
	struct mapped_ctx *ctx;
	const char *name = dirent_data;
	int flags = O_RDONLY;
	int ret;

	ctx = malloc(sizeof(struct mapped_ctx));
	if (ctx == NULL)
		return -ENOMEM;

	if (dir)
		flags |= O_DIRECTORY;

	ctx->fd = open(name, flags);
	if (ctx->fd == -1) {
		ret = -errno;
		goto err;
	}

	ctx->dir = NULL;
	ctx->writable = false;

	*fd_data = ctx;

	return 0;

err:
	free(ctx);
	return ret;
}

static int mapped_openat(struct hexagonfs_fd *dir,
			 const char *segment,
			 bool expect_dir,
			 int flags,
			 struct hexagonfs_fd **out)
{
	struct mapped_ctx *dir_ctx = dir->data;
	struct hexagonfs_fd *fd;
	struct mapped_ctx *ctx;
	bool writing = (flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC));
	struct stat st;
	int ret;
	if (writing && !dir_ctx->writable)
		return -EROFS;
	if (writing && expect_dir)
		return -EISDIR;

	ctx = malloc(sizeof(struct mapped_ctx));
	if (ctx == NULL)
		return -ENOMEM;

	fd = malloc(sizeof(struct hexagonfs_fd));
	if (fd == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	if (expect_dir)
		flags |= O_DIRECTORY;

	/* Do not follow links out of the scratch copy. Check hard links and file
	 * type before truncation so a linked original can never be modified. */
	ctx->fd = openat(dir_ctx->fd, segment,
			(flags & ~O_TRUNC) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
	if (ctx->fd == -1) {
		ret = -errno;
		goto err_free_fd;
	}
	if (fstat(ctx->fd, &st) < 0) {
		ret = -errno;
		goto err_close_fd;
	}
	if ((writing && (!S_ISREG(st.st_mode) || st.st_nlink != 1)) ||
	    (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
		ret = -EPERM;
		goto err_close_fd;
	}
	if ((flags & O_TRUNC) && ftruncate(ctx->fd, 0) < 0) {
		ret = -errno;
		goto err_close_fd;
	}

	ctx->dir = NULL;
	ctx->writable = dir_ctx->writable;

	fd->is_assigned = false;
	fd->up = dir;
	fd->ops = &hexagonfs_mapped_ops;
	fd->data = ctx;

	*out = fd;

	return 0;

err_close_fd:
	close(ctx->fd);
err_free_fd:
	free(fd);
err:
	free(ctx);
	return ret;
}

static int registry_from_dirent(const void *path, bool dir, void **out)
{
	struct mapped_ctx *ctx = calloc(1, sizeof(*ctx));
	(void)dir;
	if (!ctx)
		return -ENOMEM;
	ctx->fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (ctx->fd < 0) {
		int ret = -errno;
		free(ctx);
		return ret;
	}
	ctx->writable = true;
	*out = ctx;
	return 0;
}

static ssize_t mapped_write(struct hexagonfs_fd *fd, size_t size, const void *ptr)
{
	struct mapped_ctx *ctx = fd->data;
	ssize_t ret;
	if (!ctx->writable)
		return -EROFS;
	ret = write(ctx->fd, ptr, size);
	return ret < 0 ? -errno : ret;
}

static int mapped_removeat(struct hexagonfs_fd *dir, const char *name)
{
	struct mapped_ctx *ctx = dir->data;
	struct stat st;
	if (!ctx->writable)
		return -EROFS;
	if (fstatat(ctx->fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
		return -errno;
	if (!S_ISREG(st.st_mode) || st.st_nlink != 1)
		return -EPERM;
	return unlinkat(ctx->fd, name, 0) < 0 ? -errno : 0;
}

/*
 * frename within the writable registry copy: rename(2) semantics, so an existing
 * target is replaced atomically. Source and an existing target must be regular
 * files with one link; links are never followed. The source data and both
 * directories are synced, so a completed rename survives a power loss.
 */
static int mapped_renameat(struct hexagonfs_fd *olddir, const char *oldname,
			   struct hexagonfs_fd *newdir, const char *newname)
{
	struct mapped_ctx *from = olddir->data, *to = newdir->data;
	struct stat st;
	int fd, ret = 0;
	if (!from || !to || !from->writable || !to->writable)
		return -EROFS;
	fd = openat(from->fd, oldname, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0)
		ret = -errno;
	else if (!S_ISREG(st.st_mode) || st.st_nlink != 1)
		ret = -EPERM;
	else if (fsync(fd) < 0)
		ret = -errno;
	close(fd);
	if (ret)
		return ret;
	if (fstatat(to->fd, newname, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		if (!S_ISREG(st.st_mode) || st.st_nlink != 1)
			return -EPERM;
	} else if (errno != ENOENT) {
		return -errno;
	}
	if (renameat(from->fd, oldname, to->fd, newname) < 0)
		return -errno;
	if (fsync(to->fd) < 0 || (from->fd != to->fd && fsync(from->fd) < 0))
		return -errno;
	return 0;
}

static ssize_t mapped_read(struct hexagonfs_fd *fd, size_t size, void *out)
{
	struct mapped_ctx *ctx = fd->data;
	ssize_t ret;

	ret = read(ctx->fd, out, size);
	if (ret < 0)
		return -errno;

	return ret;
}

static int mapped_readdir(struct hexagonfs_fd *fd, size_t size, char *out)
{
	struct mapped_ctx *ctx = fd->data;
	struct dirent *ent;

	if (ctx->dir == NULL) {
		ctx->dir = fdopendir(ctx->fd);
		if (ctx->dir == NULL)
			return -errno;

		ctx->fd = dirfd(ctx->dir);
		if (ctx->fd == -1)
			return -errno;
	}

	errno = 0;

	ent = readdir(ctx->dir);
	if (ent == NULL) {
		out[0] = '\0';
		return -errno;
	}

	strncpy(out, ent->d_name, size);
	out[size - 1] = '\0';

	return 0;
}

static int mapped_seek(struct hexagonfs_fd *fd, off_t off, int whence)
{
	struct mapped_ctx *ctx = fd->data;
	int ret;

	ret = lseek(ctx->fd, off, whence);
	if (ret == -1)
		return -errno;

	return 0;
}

static int mapped_stat(struct hexagonfs_fd *fd, struct stat *stats)
{
	struct mapped_ctx *ctx = fd->data;
	struct stat phys;
	int ret;

	ret = fstat(ctx->fd, &phys);
	if (ret)
		return -errno;

	stats->st_size = phys.st_size;

	stats->st_dev = 0;
	stats->st_rdev = 0;

	stats->st_ino = 0;
	stats->st_nlink = 0;

	if (S_ISDIR(phys.st_mode)) {
		stats->st_mode = S_IFDIR
			       | S_IRUSR | S_IXUSR
			       | S_IRGRP | S_IXGRP
			       | S_IROTH | S_IXOTH;
	} else {
		stats->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
	}
	if (ctx->writable)
		stats->st_mode |= S_IWUSR;

	stats->st_atim.tv_sec = phys.st_atim.tv_sec;
	stats->st_atim.tv_nsec = phys.st_atim.tv_nsec;
	stats->st_ctim.tv_sec = phys.st_ctim.tv_sec;
	stats->st_ctim.tv_nsec = phys.st_ctim.tv_nsec;
	stats->st_mtim.tv_sec = phys.st_mtim.tv_sec;
	stats->st_mtim.tv_nsec = phys.st_mtim.tv_nsec;

	return 0;
}

static void mapped_or_empty_close(void *fd_data)
{
	if (fd_data)
		mapped_close(fd_data);
}

static int mapped_or_empty_from_dirent(const void *dirent_data, bool dir, void **fd_data)
{
	int ret;

	ret = mapped_from_dirent(dirent_data, dir, fd_data);
	if (ret)
		*fd_data = NULL;

	return 0;
}

static int mapped_or_empty_openat(struct hexagonfs_fd *dir,
				  const char *segment,
				  bool expect_dir,
				  int flags,
				  struct hexagonfs_fd **out)
{
	if (dir->data)
		return mapped_openat(dir, segment, expect_dir, flags, out);
	else
		return -ENOENT;
}

static ssize_t mapped_or_empty_read(struct hexagonfs_fd *fd,
				    size_t size, void *out)
{
	if (fd->data)
		return mapped_read(fd, size, out);

	return 0;
}

static int mapped_or_empty_readdir(struct hexagonfs_fd *fd,
				   size_t size, char *out)
{
	if (fd->data) {
		return mapped_readdir(fd, size, out);
	} else {
		out[0] = '\0';
		return 0;
	}
}

static int mapped_or_empty_seek(struct hexagonfs_fd *fd, off_t off, int whence)
{
	if (fd->data)
		return mapped_seek(fd, off, whence);
	else
		return 0;
}

static int mapped_or_empty_stat(struct hexagonfs_fd *fd, struct stat *stats)
{
	if (fd->data) {
		return mapped_stat(fd, stats);
	} else {
		stats->st_size = 0;
		stats->st_dev = 0;
		stats->st_rdev = 0;
		stats->st_ino = 0;
		stats->st_nlink = 0;

		stats->st_mode = S_IFDIR
			       | S_IRUSR | S_IXUSR
			       | S_IRGRP | S_IXGRP
			       | S_IROTH | S_IXOTH;

		stats->st_atim.tv_sec = 0;
		stats->st_atim.tv_nsec = 0;
		stats->st_ctim.tv_sec = 0;
		stats->st_ctim.tv_nsec = 0;
		stats->st_mtim.tv_sec = 0;
		stats->st_mtim.tv_nsec = 0;

		return 0;
	}
}

/*
 * This is the sysfs variant of the stat function.
 *
 * The remote processor expects a non-zero size if the file is not empty, even
 * if a size cannot be determined without reading. The size is 256 on
 * downstream kernels, so set it to that.
 *
 * Otherwise, the existing stat() function can be used.
 */
static int mapped_sysfs_stat(struct hexagonfs_fd *fd, struct stat *stats)
{
	int ret;

	ret = mapped_stat(fd, stats);
	if (ret)
		return ret;

	if (!(stats->st_mode & S_IFDIR))
		stats->st_size = 256;

	return 0;
}

struct hexagonfs_file_ops hexagonfs_mapped_ops = {
	.close = mapped_close,
	.from_dirent = mapped_from_dirent,
	.openat = mapped_openat,
	.read = mapped_read,
	.readdir = mapped_readdir,
	.seek = mapped_seek,
	.stat = mapped_stat,
	.write = mapped_write,
	.removeat = mapped_removeat,
	.renameat = mapped_renameat,
};

struct hexagonfs_file_ops hexagonfs_registry_ops = {
	.close = mapped_close,
	.from_dirent = registry_from_dirent,
	.openat = mapped_openat,
	.read = mapped_read,
	.readdir = mapped_readdir,
	.seek = mapped_seek,
	.stat = mapped_stat,
	.write = mapped_write,
	.removeat = mapped_removeat,
	.renameat = mapped_renameat,
};

struct hexagonfs_file_ops hexagonfs_mapped_or_empty_ops = {
	.close = mapped_or_empty_close,
	.from_dirent = mapped_or_empty_from_dirent,
	.openat = mapped_or_empty_openat,
	.read = mapped_or_empty_read,
	.readdir = mapped_or_empty_readdir,
	.seek = mapped_or_empty_seek,
	.stat = mapped_or_empty_stat,
};

struct hexagonfs_file_ops hexagonfs_mapped_sysfs_ops = {
	.close = mapped_close,
	.from_dirent = mapped_from_dirent,
	.openat = mapped_openat,
	.read = mapped_read,
	.readdir = mapped_readdir,
	.seek = mapped_seek,
	.stat = mapped_sysfs_stat,
};
