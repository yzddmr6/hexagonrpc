/*
 * Virtual read-only filesystem for Hexagon processors
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

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "hexagonfs.h"

static char *copy_segment_and_advance(const char *path,
				      bool *trailing_slash,
				      const char **next)
{
	const char *next_tmp;
	char *segment;
	size_t segment_len;

	next_tmp = strchr(path, '/');
	if (next_tmp == NULL) {
		next_tmp = &path[strlen(path)];
		*trailing_slash = false;
	} else {
		*trailing_slash = true;
	}

	segment_len = next_tmp - path;

	while (*next_tmp == '/')
		next_tmp++;

	segment = malloc(segment_len + 1);
	if (segment == NULL)
		return NULL;

	memcpy(segment, path, segment_len);
	segment[segment_len] = 0;

	*next = next_tmp;

	return segment;
}

static struct hexagonfs_fd *pop_dir(struct hexagonfs_fd *dir,
				    struct hexagonfs_fd *root)
{
	struct hexagonfs_fd *up;

	if (dir != root && dir->up != NULL) {
		up = dir->up;

		if (!dir->is_assigned) {
			dir->ops->close(dir->data);
			free(dir);
		}
	} else {
		up = dir;
	}

	return up;
}

static int allocate_file_number(struct hexagonfs_fd **fds,
				struct hexagonfs_fd *fd)
{
	size_t i;

	for (i = 0; i < HEXAGONFS_MAX_FD; i++) {
		if (fds[i] == NULL) {
			fd->is_assigned = true;
			fds[i] = fd;
			return i;
		}
	}

	return -EMFILE;
}

static void destroy_file_descriptor(struct hexagonfs_fd *fd)
{
	struct hexagonfs_fd *curr = fd;
	struct hexagonfs_fd *next;

	while (curr != NULL && !curr->is_assigned) {
		next = curr->up;
		curr->ops->close(curr->data);
		free(curr);

		curr = next;
	}
}

int hexagonfs_open_root(struct hexagonfs_fd **fds, struct hexagonfs_dirent *root)
{
	struct hexagonfs_fd *fd;
	int ret;

	fd = malloc(sizeof(struct hexagonfs_fd));
	if (fd == NULL)
		return -ENOMEM;

	fd->is_assigned = false;
	fd->up = NULL;
	fd->ops = root->ops;

	ret = root->ops->from_dirent(root->u.ptr, true, &fd->data);
	if (ret)
		goto err;

	ret = allocate_file_number(fds, fd);
	if (ret < 0)
		goto err;

	return ret;

err:
	destroy_file_descriptor(fd);
	return ret;
}

int hexagonfs_openat_flags(struct hexagonfs_fd **fds, int rootfd, int dirfd, const char *name, int flags)
{
	struct hexagonfs_fd *fd;
	const char *curr = name;
	char *segment;
	bool expect_dir;
	int selected = dirfd;
	int ret = 0;

	if (*curr == '/') {
		selected = rootfd;

		while (*curr == '/')
			curr++;
	}

	if (selected < 0 || selected >= HEXAGONFS_MAX_FD || !fds[selected])
		return -EBADF;
	fd = fds[selected];

	while (*curr != '\0' && !ret) {
		segment = copy_segment_and_advance(curr, &expect_dir, &curr);
		if (segment == NULL) {
			ret = -ENOMEM;
			goto err;
		}

		if (!strcmp(segment, ".")) {
			if (flags != O_RDONLY && !*curr) {
				ret = -EISDIR;
				goto next;
			}
			goto next;
		} else if (!strcmp(segment, "..")) {
			if (flags != O_RDONLY && !*curr) {
				ret = -EISDIR;
				goto next;
			}
			fd = pop_dir(fd, fds[rootfd]);
		} else {
			ret = fd->ops->openat(fd, segment, expect_dir,
					      *curr ? O_RDONLY : flags, &fd);
		}

	next:
		free(segment);
	}

	if (ret)
		goto err;

	/* Do not alias an already-owned descriptor for empty paths or root/.. . */
	if (fd->is_assigned)
		return -EINVAL;
	ret = allocate_file_number(fds, fd);
	if (ret < 0)
		goto err;

	return ret;

err:
	destroy_file_descriptor(fd);

	return ret;
}

int hexagonfs_openat(struct hexagonfs_fd **fds, int rootfd, int dirfd, const char *name)
{
	return hexagonfs_openat_flags(fds, rootfd, dirfd, name, O_RDONLY);
}

ssize_t hexagonfs_write(struct hexagonfs_fd **fds, int fileno, size_t size, const void *ptr)
{
	if (fileno < 0 || fileno >= HEXAGONFS_MAX_FD || !fds[fileno])
		return -EBADF;
	if (!fds[fileno]->ops->write)
		return -EROFS;
	return fds[fileno]->ops->write(fds[fileno], size, ptr);
}

/*
 * Resolve the directory holding the last segment of PATH with the normal
 * mapping rules (absolute from the root, otherwise from DIRFD; ".." stays in
 * the virtual tree). PATH is split in place and *LEAF points to the segment.
 */
static int open_parent(struct hexagonfs_fd **fds, int rootfd, int dirfd, char *path, char **leaf)
{
	*leaf = strrchr(path, '/');
	if (!*leaf) {
		*leaf = path;
		return dirfd;
	}
	*(*leaf)++ = '\0';
	return hexagonfs_openat(fds, rootfd, dirfd, *path ? path : "/");
}

static void close_parent(struct hexagonfs_fd **fds, int rootfd, int dirfd,
			 const char *path, const char *leaf, int parent)
{
	if (leaf != path && parent >= 0 && parent != dirfd && parent != rootfd)
		hexagonfs_close(fds, parent);
}

static int check_parent(struct hexagonfs_fd **fds, const char *leaf, int parent)
{
	if (!*leaf || !strcmp(leaf, ".") || !strcmp(leaf, ".."))
		return -EINVAL;
	if (parent < 0 || parent >= HEXAGONFS_MAX_FD || !fds[parent])
		return parent < 0 ? parent : -EBADF;
	return 0;
}

int hexagonfs_removeat(struct hexagonfs_fd **fds, int rootfd, int dirfd, const char *name)
{
	char *path = strdup(name), *leaf;
	int parent, ret;
	struct hexagonfs_fd *dir;
	if (!path)
		return -ENOMEM;
	parent = open_parent(fds, rootfd, dirfd, path, &leaf);
	ret = check_parent(fds, leaf, parent);
	if (!ret) {
		dir = fds[parent];
		ret = dir->ops->removeat ? dir->ops->removeat(dir, leaf) : -EROFS;
	}
	close_parent(fds, rootfd, dirfd, path, leaf, parent);
	free(path);
	return ret;
}

/*
 * Both names are resolved like removeat. Only directories whose ops share one
 * renameat implementation (the explicitly writable registry copy) can take part;
 * every other mapping, including what ".." escapes reach, is read-only.
 */
int hexagonfs_renameat(struct hexagonfs_fd **fds, int rootfd, int dirfd,
		       const char *oldname, const char *newname)
{
	char *oldpath = strdup(oldname), *newpath = strdup(newname), *oldleaf, *newleaf;
	int oldparent, newparent, ret;
	struct hexagonfs_fd *olddir, *newdir;
	if (!oldpath || !newpath) {
		free(oldpath);
		free(newpath);
		return -ENOMEM;
	}
	oldparent = open_parent(fds, rootfd, dirfd, oldpath, &oldleaf);
	newparent = open_parent(fds, rootfd, dirfd, newpath, &newleaf);
	ret = check_parent(fds, oldleaf, oldparent);
	if (!ret)
		ret = check_parent(fds, newleaf, newparent);
	if (!ret) {
		olddir = fds[oldparent];
		newdir = fds[newparent];
		if (!olddir->ops->renameat || olddir->ops->renameat != newdir->ops->renameat)
			ret = -EROFS;
		else
			ret = olddir->ops->renameat(olddir, oldleaf, newdir, newleaf);
	}
	close_parent(fds, rootfd, dirfd, newpath, newleaf, newparent);
	close_parent(fds, rootfd, dirfd, oldpath, oldleaf, oldparent);
	free(newpath);
	free(oldpath);
	return ret;
}

int hexagonfs_close(struct hexagonfs_fd **fds, int fileno)
{
	struct hexagonfs_fd *fd;

	if (fileno < 0 || fileno >= HEXAGONFS_MAX_FD)
		return -EBADF;

	fd = fds[fileno];
	if (fd == NULL || fd->ops == NULL)
		return -EBADF;

	fd->is_assigned = false;
	destroy_file_descriptor(fd);

	fds[fileno] = NULL;

	return 0;
}

int hexagonfs_lseek(struct hexagonfs_fd **fds, int fileno, off_t off, int whence)
{
	struct hexagonfs_fd *fd;

	if (fileno < 0 || fileno >= HEXAGONFS_MAX_FD)
		return -EBADF;

	fd = fds[fileno];
	if (fd == NULL)
		return -EBADF;

	if (fd->ops->seek == NULL)
		return -ENOSYS;

	return fd->ops->seek(fd, off, whence);
}

ssize_t hexagonfs_read(struct hexagonfs_fd **fds, int fileno, size_t size, void *ptr)
{
	struct hexagonfs_fd *fd;

	if (fileno < 0 || fileno >= HEXAGONFS_MAX_FD)
		return -EBADF;

	fd = fds[fileno];
	if (fd == NULL)
		return -EBADF;

	if (fd->ops->read == NULL)
		return -ENOSYS;

	return fd->ops->read(fd, size, ptr);
}

int hexagonfs_readdir(struct hexagonfs_fd **fds, int fileno, size_t ent_size, char *ent)
{
	struct hexagonfs_fd *fd;

	if (fileno < 0 || fileno >= HEXAGONFS_MAX_FD)
		return -EBADF;

	fd = fds[fileno];
	if (fd == NULL)
		return -EBADF;

	if (fd->ops->readdir == NULL)
		return -ENOSYS;

	return fd->ops->readdir(fd, ent_size, ent);
}

int hexagonfs_fstat(struct hexagonfs_fd **fds, int fileno, struct stat *stats)
{
	struct hexagonfs_fd *fd;

	if (fileno < 0 || fileno >= HEXAGONFS_MAX_FD)
		return -EBADF;

	fd = fds[fileno];
	if (fd == NULL)
		return -EBADF;

	if (fd->ops->stat == NULL)
		return -ENOSYS;

	return fd->ops->stat(fd, stats);
}
