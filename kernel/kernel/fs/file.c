/*
* Copyright (c) 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>

#include <partitions.h>

#include <onyx/compiler.h>
#include <onyx/vm.h>
#include <onyx/vfs.h>
#include <onyx/process.h>
#include <onyx/pipe.h>
#include <onyx/file.h>
#include <onyx/atomic.h>
#include <onyx/user.h>
#include <onyx/panic.h>
#include <onyx/dentry.h>

#include <libgen.h>

#include <sys/uio.h>

bool is_absolute_filename(const char *file)
{
	return *file == '/' ? true : false;
}

struct file *get_fs_base(const char *file, struct file *rel_base)
{
	return is_absolute_filename(file) == true ? get_fs_root() : rel_base;
}

struct file *get_current_directory(void)
{
	struct ioctx *ctx = &get_current_process()->ctx;
	spin_lock(&ctx->cwd_lock);

	struct file *fp = ctx->cwd;

	if(unlikely(!fp))
	{
		spin_unlock(&ctx->cwd_lock);
		return NULL;
	}

	fd_get(fp);

	spin_unlock(&ctx->cwd_lock);
	return fp;
}

void fd_get(struct file *fd)
{
	__sync_add_and_fetch(&fd->f_refcount, 1);
}

void fd_put(struct file *fd)
{
	if(__sync_sub_and_fetch(&fd->f_refcount, 1) == 0)
	{
		close_vfs(fd->f_ino);
		dentry_put(fd->f_dentry);
		free(fd);
	}
}

static bool validate_fd_number(int fd, struct ioctx *ctx)
{
	if(fd < 0)
	{
		return false;
	}

	if((unsigned int) fd >= ctx->file_desc_entries)
	{
		return false;
	}
	
	if(ctx->file_desc[fd] == NULL)
	{
		return false;
	}

	return true;
}

static inline bool fd_is_open(int fd, struct ioctx *ctx)
{
	unsigned long long_idx = fd / FDS_PER_LONG;
	unsigned long bit_idx = fd % FDS_PER_LONG;
	return ctx->open_fds[long_idx] & (1 << bit_idx);
}

static inline void fd_close_bit(int fd, struct ioctx *ctx)
{
	unsigned long long_idx = fd / FDS_PER_LONG;
	unsigned long bit_idx = fd % FDS_PER_LONG;
	ctx->open_fds[long_idx] &= ~(1 << bit_idx);
}

void fd_set_cloexec(int fd, bool toggle, struct ioctx *ctx)
{
	unsigned long long_idx = fd / FDS_PER_LONG;
	unsigned long bit_idx = fd % FDS_PER_LONG;

	if(toggle)
		ctx->cloexec_fds[long_idx] |= (1 << bit_idx);
	else
		ctx->cloexec_fds[long_idx] &= ~(1 << bit_idx);
}

void fd_set_open(int fd, bool toggle, struct ioctx *ctx)
{
	unsigned long long_idx = fd / FDS_PER_LONG;
	unsigned long bit_idx = fd % FDS_PER_LONG;

	if(toggle)
		ctx->open_fds[long_idx] |= (1 << bit_idx);
	else
		ctx->open_fds[long_idx] &= ~(1 << bit_idx);
}

bool fd_is_cloexec(int fd, struct ioctx *ctx)
{
	unsigned long long_idx = fd / FDS_PER_LONG;
	unsigned long bit_idx = fd % FDS_PER_LONG;

	return ctx->cloexec_fds[long_idx] & (1 << bit_idx);
}

struct file *__get_file_description(int fd, struct process *p)
{
	struct ioctx *ctx = &p->ctx;

	mutex_lock(&ctx->fdlock);

	if(!validate_fd_number(fd, ctx))
		goto badfd;

	struct file *f = ctx->file_desc[fd];
	fd_get(f);

	mutex_unlock(&ctx->fdlock);

	return f;
badfd:
	mutex_unlock(&ctx->fdlock);
	return errno = EBADF, NULL;
}

int __file_close_unlocked(int fd, struct process *p)
{
	struct ioctx *ctx = &p->ctx;

	if(!validate_fd_number(fd, ctx))
		goto badfd;

	struct file *f = ctx->file_desc[fd];
	
	/* Decrement the ref count and set the entry to NULL */
	/* TODO: Shrink the fd table? */
	fd_put(f);

	ctx->file_desc[fd] = NULL;
	fd_close_bit(fd, ctx);
	
	return 0;
badfd:
	return -EBADF;
}

int __file_close(int fd, struct process *p)
{
	struct ioctx *ctx = &p->ctx;

	mutex_lock(&ctx->fdlock);

	int ret = __file_close_unlocked(fd, p);

	mutex_unlock(&ctx->fdlock);

	return ret;
}

int file_close(int fd)
{
	return __file_close(fd, get_current_process());
}

struct file *get_file_description(int fd)
{
	return __get_file_description(fd, get_current_process());
}

int copy_file_descriptors(struct process *process, struct ioctx *ctx)
{
	mutex_lock(&ctx->fdlock);

	process->ctx.file_desc = malloc(ctx->file_desc_entries * sizeof(void*));
	process->ctx.file_desc_entries = ctx->file_desc_entries;
	if(!process->ctx.file_desc)
	{
		mutex_unlock(&ctx->fdlock);
		return -ENOMEM;
	}

	process->ctx.cloexec_fds = malloc(ctx->file_desc_entries / 8);
	if(!process->ctx.cloexec_fds)
	{
		free(process->ctx.file_desc);
		return -ENOMEM;
	}

	process->ctx.open_fds = malloc(ctx->file_desc_entries / 8);
	if(!process->ctx.open_fds)
	{
		free(process->ctx.file_desc);
		free(process->ctx.cloexec_fds);
		return -ENOMEM;
	}

	for(unsigned int i = 0; i < process->ctx.file_desc_entries; i++)
	{
		process->ctx.file_desc[i] = ctx->file_desc[i];
		if(ctx->file_desc[i])
			fd_get(ctx->file_desc[i]);
	}

	memcpy(process->ctx.cloexec_fds, ctx->cloexec_fds, ctx->file_desc_entries / 8);
	memcpy(process->ctx.open_fds, ctx->open_fds, ctx->file_desc_entries / 8);

	mutex_unlock(&ctx->fdlock);
	return 0;
}

int allocate_file_descriptor_table(struct process *process)
{
	process->ctx.file_desc = zalloc(FILE_DESCRIPTOR_GROW_NR * sizeof(void*));
	if(!process->ctx.file_desc)
		return -ENOMEM;

	process->ctx.file_desc_entries = FILE_DESCRIPTOR_GROW_NR;
	
	process->ctx.cloexec_fds = zalloc(FILE_DESCRIPTOR_GROW_NR / 8);
	if(!process->ctx.cloexec_fds)
	{
		free(process->ctx.file_desc);
		return -ENOMEM;
	}

	process->ctx.open_fds = zalloc(FILE_DESCRIPTOR_GROW_NR / 8);
	if(!process->ctx.open_fds)
	{
		free(process->ctx.file_desc);
		free(process->ctx.cloexec_fds);
		return -1;
	}

	return 0;
}

#define FD_ENTRIES_TO_FDSET_SIZE(x)				((x) / 8)

/* Enlarges the file descriptor table by FILE_DESCRIPTOR_GROW_NR(64) entries */
int enlarge_file_descriptor_table(struct process *process, unsigned int new_size)
{
	unsigned int old_nr_fds = process->ctx.file_desc_entries;

	new_size = ALIGN_TO(new_size, FILE_DESCRIPTOR_GROW_NR);

	process->ctx.file_desc_entries = new_size;

	if(new_size > INT_MAX)
		return -EMFILE;

	unsigned int new_nr_fds = process->ctx.file_desc_entries;

	struct file **table = malloc(process->ctx.file_desc_entries * sizeof(void*));
	unsigned long *cloexec_fds = malloc(FD_ENTRIES_TO_FDSET_SIZE(new_nr_fds));
	/* We use zalloc here to implicitly zero free fds */
	unsigned long *open_fds = zalloc(FD_ENTRIES_TO_FDSET_SIZE(new_nr_fds));
	if(!table || !cloexec_fds || !open_fds)
		goto error;
	
	/* Note that we use old_nr_fds for these copies specifically as to not go
	 * out of bounds.
	 */
	memcpy(table, process->ctx.file_desc, (old_nr_fds) * sizeof(void*));
	memcpy(cloexec_fds, process->ctx.cloexec_fds, FD_ENTRIES_TO_FDSET_SIZE(old_nr_fds));
	memcpy(open_fds, process->ctx.open_fds, FD_ENTRIES_TO_FDSET_SIZE(old_nr_fds));

	free(process->ctx.cloexec_fds);
	free(process->ctx.open_fds);
	free(process->ctx.file_desc);

	process->ctx.file_desc = table;
	process->ctx.cloexec_fds = cloexec_fds;
	process->ctx.open_fds = open_fds;

	return 0;

error:
	free(table);
	free(cloexec_fds);
	free(open_fds);

	/* Don't forget to restore the old file_desc_entries! */
	process->ctx.file_desc_entries = old_nr_fds;

	return -ENOMEM;
}

int alloc_fd(int fdbase)
{
	struct ioctx *ioctx = &get_current_process()->ctx;
	mutex_lock(&ioctx->fdlock);

	unsigned long starting_long = fdbase / FDS_PER_LONG;

	while(true)
	{
		unsigned long nr_longs = ioctx->file_desc_entries / FDS_PER_LONG;

		for(unsigned long i = starting_long; i < nr_longs; i++)
		{
			if(ioctx->open_fds[i] == ULONG_MAX)
				continue;
			
			/* We speed it up by doing an ffz. */
			unsigned int first_free = __builtin_ctz(~ioctx->open_fds[i]);
	
			for(unsigned int j = first_free; j < FDS_PER_LONG; j++)
			{
				int fd = FDS_PER_LONG * i + j;

				if(ioctx->open_fds[i] & (1 << j))
					continue;

				if(fd < fdbase)
					continue;
				else
				{
					/* Found a free fd that we can use, let's mark it used and return it */
					ioctx->open_fds[i] |= (1 << j);
					/* And don't forget to reset the cloexec flag! */
					fd_set_cloexec(fd, false, ioctx); 
					return fd;
				} 
			}
		}

		/* TODO: Make it so we can enlarge it directly to the size we want */
		int new_entries = ioctx->file_desc_entries + FILE_DESCRIPTOR_GROW_NR;
		if(enlarge_file_descriptor_table(get_current_process(), new_entries) < 0)
		{
			mutex_unlock(&ioctx->fdlock);
			return -ENOMEM;
		}
	}
}

int file_alloc(struct file *f, struct ioctx *ioctx)
{
	int filedesc = alloc_fd(0);
	if(filedesc < 0)
		return errno = -filedesc, filedesc;
	
	ioctx->file_desc[filedesc] = f;
	fd_get(f);

	return filedesc;
}

ssize_t sys_read(int fd, const void *buf, size_t count)
{
	struct file *f = get_file_description(fd);
	if(!f)
		goto error;
	
	if(!fd_may_access(f, FILE_ACCESS_READ))
	{
		errno = EBADF;
		goto error;
	}

	ssize_t size = read_vfs(f->f_seek,
		count, (char*) buf, f);
	if(size == -1)
	{
		goto error;
	}

	/* TODO: Seek adjustments are required to be atomic */
	__sync_add_and_fetch(&f->f_seek, size);
	fd_put(f);

	return size;
error:
	if(f) fd_put(f);
	return -errno;
}

ssize_t sys_write(int fd, const void *buf, size_t count)
{	
	struct file *f = get_file_description(fd);
	if(!f)
		goto error;

	if(!fd_may_access(f, FILE_ACCESS_WRITE))
	{
		errno = EBADF;
		goto error;
	}

	if(f->f_flags & O_APPEND)
		f->f_seek = f->f_ino->i_size;
	
	size_t written = write_vfs(f->f_seek,
				   count, (void*) buf, 
				   f);

	if(written == (size_t) -1)
		goto error;

	__sync_add_and_fetch(&f->f_seek, written);

	fd_put(f);
	return written;
error:
	if(f) fd_put(f);
	return -errno;
}

ssize_t sys_pread(int fd, void *buf, size_t count, off_t offset)
{
	struct file *f = get_file_description(fd);
	if(!f)
		goto error;
	
	if(!fd_may_access(f, FILE_ACCESS_READ))
	{
		errno = EBADF;
		goto error;
	}

	if(offset < 0)
	{
		errno = EINVAL;
		return -errno;
	}

	ssize_t size = read_vfs(offset,
		count, (char*) buf, f);
	if(size < 0)
	{
		goto error;
	}

	fd_put(f);

	return size;
error:
	if(f) fd_put(f);
	return -errno;
}

ssize_t sys_pwrite(int fd, const void *buf, size_t count, off_t offset)
{	
	struct file *f = get_file_description(fd);
	if(!f)
		goto error;

	if(!fd_may_access(f, FILE_ACCESS_WRITE))
	{
		errno = EBADF;
		goto error;
	}

	if(offset < 0)
	{
		errno = EINVAL;
		goto error;
	}
	
	ssize_t written = write_vfs(offset,
				   count, (void*) buf, 
				   f);

	if(written < 0)
		goto error;


	fd_put(f);
	return written;
error:
	if(f) fd_put(f);
	return -errno;
}


void handle_open_flags(struct file *fd, int flags)
{
	if(flags & O_APPEND)
		fd->f_seek = fd->f_ino->i_size;
}

static inline mode_t get_current_umask(void)
{
	return get_current_process()->ctx.umask;
}

static struct file *try_to_open(struct file *base, const char *filename, int flags, mode_t mode)
{
	unsigned int open_flags = (flags & O_EXCL ? OPEN_FLAG_FAIL_IF_LINK : 0) |
	                          (flags & O_NOFOLLOW ? OPEN_FLAG_FAIL_IF_LINK : 0) |
							  (flags & O_DIRECTORY ? OPEN_FLAG_MUST_BE_DIR : 0);
	struct file *ret = open_vfs_with_flags(base, filename, open_flags);
	
	if(ret)
	{
		/* Let's check for permissions */
		if(!file_can_access(ret, open_to_file_access_flags(flags)))
		{
			fd_put(ret);
			return errno = EACCES, NULL;
		}

		if(ret->f_ino->i_type == VFS_TYPE_DIR)
		{
			if(flags & O_RDWR || flags & O_WRONLY || (flags & O_CREAT && !(flags & O_DIRECTORY)))
			{
				fd_put(ret);
				return errno = EISDIR, NULL;
			}
		}

		if(flags & O_EXCL)
		{
			fd_put(ret);
			return errno = EEXIST, NULL;
		}
	}

	if(!ret && errno == ENOENT && flags & O_CREAT)
		ret = creat_vfs(base->f_dentry, filename, mode & ~get_current_umask());

	return ret;
}

/* TODO: Add O_PATH */
/* TODO: Add O_TRUNC */
/* TODO: Add O_SYNC */
#define VALID_OPEN_FLAGS      (O_RDONLY | O_WRONLY | O_RDWR | \
                               O_CREAT | O_DIRECTORY | O_EXCL | \
                               O_NOFOLLOW | O_NONBLOCK | O_APPEND | O_CLOEXEC | O_LARGEFILE)

int do_sys_open(const char *filename, int flags, mode_t mode, struct file *__rel)
{
	if(flags & ~VALID_OPEN_FLAGS)
		return -EINVAL;

	//printk("Open(%s)\n", filename);
	/* This function does all the open() work, open(2) and openat(2) use this */
	struct file *rel = __rel;
	struct file *base = get_fs_base(filename, rel);

	int fd_num = -1;

	/* Open/creat the file */
	struct file *file = try_to_open(base, filename, flags, mode);
	if(!file)
	{
		return -errno;
	}

	/* Allocate a file descriptor and a file description for the file */
	fd_num = open_with_vnode(file, flags);

	fd_put(file);

	return fd_num;
}

int sys_open(const char *ufilename, int flags, mode_t mode)
{
	const char *filename = strcpy_from_user(ufilename);
	if(!filename)
		return -errno;
	struct file *cwd = get_current_directory();
	/* TODO: Unify open and openat better */
	/* open(2) does relative opens using the current working directory */
	int fd = do_sys_open(filename, flags, mode, cwd);
	free((char *) filename);
	fd_put(cwd);
	return fd;
}

int sys_close(int fd)
{
	return file_close(fd);
}

int sys_dup(int fd)
{
	int st = 0;
	struct ioctx *ioctx = &get_current_process()->ctx;
	
	struct file *f = get_file_description(fd);
	if(!f)
		return -errno;

	int new_fd = alloc_fd(0);

	if(new_fd < 0)
	{
		st = new_fd;
		goto out_error;
	}

	ioctx->file_desc[new_fd] = f;

	/* We don't put the fd on success, because it's the reference the new fd holds */

	mutex_unlock(&ioctx->fdlock);

	return new_fd;
out_error:
	fd_put(f);
	return st;
}

int sys_dup2(int oldfd, int newfd)
{
	struct process *current = get_current_process();
	struct ioctx *ioctx = &current->ctx;

	if(newfd < 0)
		return -EINVAL;

	struct file *f = get_file_description(oldfd);
	if(!f)
		return -errno;

	mutex_lock(&ioctx->fdlock);
	if((unsigned int) newfd > ioctx->file_desc_entries)
	{
		int st = enlarge_file_descriptor_table(current, newfd + 1);
		if(st < 0)
		{
			fd_put(f);
			return st;
		}
	}

	if(ioctx->file_desc[newfd])
		__file_close_unlocked(newfd, current);

	ioctx->file_desc[newfd] = ioctx->file_desc[oldfd];
	fd_set_cloexec(newfd, false, ioctx);
	fd_set_open(newfd, true, ioctx);
	/* Note: To avoid fd_get/fd_put, we use the ref we get from
	 * get_file_description as the ref for newfd. Therefore, we don't
	 * fd_get and fd_put().
	*/

	mutex_unlock(&ioctx->fdlock);

	return newfd;
}

bool fd_may_access(struct file *f, unsigned int access)
{
	if(access == FILE_ACCESS_READ)
	{
		if(OPEN_FLAGS_ACCESS_MODE(f->f_flags) == O_WRONLY)
			return false;
	}
	else if(access == FILE_ACCESS_WRITE)
	{
		if(OPEN_FLAGS_ACCESS_MODE(f->f_flags) == O_RDONLY)
			return false;
	}

	return true;
}

ssize_t sys_readv(int fd, const struct iovec *vec, int veccnt)
{
	size_t read = 0;

	struct file *f = get_file_description(fd);
	if(!f)
		goto error;

	if(!vec)
	{
		errno = EINVAL;
		goto error;
	}

	if(veccnt == 0)
	{
		read = 0;
		goto out;
	}

	if(!fd_may_access(f, FILE_ACCESS_READ))
	{
		errno = EBADF;
		goto error;
	}

	for(int i = 0; i < veccnt; i++)
	{
		struct iovec v;
		if(copy_from_user(&v, vec++, sizeof(struct iovec)) < 0)
		{
			errno = EFAULT;
			goto error;
		}
	
		if(v.iov_len == 0)
			continue;
		ssize_t was_read = read_vfs(f->f_seek, v.iov_len, v.iov_base, f);
		if(was_read < 0)
		{
			goto out;
		}

		read += was_read;
		f->f_seek += was_read;

		if((size_t) was_read != v.iov_len)
		{
			goto out;
		}
	}

out:
	fd_put(f);

	return read;
error:
	if(f)	fd_put(f);
	return -errno;
}

ssize_t sys_writev(int fd, const struct iovec *vec, int veccnt)
{
	size_t written = 0;

	struct file *f = get_file_description(fd);
	if(!f)
		goto error;

	if(!vec)
	{
		errno = EINVAL;
		goto error;
	}

	if(veccnt == 0)
	{
		written = 0;
		goto out;
	}

	if(!fd_may_access(f, FILE_ACCESS_WRITE))
	{
		errno = EBADF;
		goto error;
	}

	for(int i = 0; i < veccnt; i++)
	{
		struct iovec v;
		if(copy_from_user(&v, vec++, sizeof(struct iovec)) < 0)
		{
			errno = EFAULT;
			goto error;
		}
	
		if(v.iov_len == 0)
			continue;

		if(f->f_flags & O_APPEND)
			f->f_seek = f->f_ino->i_size;

		size_t was_written = write_vfs(f->f_seek,
			v.iov_len, v.iov_base,f);

		written += was_written;
		f->f_seek += was_written;

		if(was_written != v.iov_len)
		{
			goto out;
		}
	}

out:
	fd_put(f);

	return written;
error:
	if(f)	fd_put(f);
	return -errno;
}

ssize_t sys_preadv(int fd, const struct iovec *vec, int veccnt, off_t offset)
{
		size_t read = 0;

	struct file *f = get_file_description(fd);
	if(!f)
		goto error;

	if(!vec)
	{
		errno = EINVAL;
		goto error;
	}

	if(veccnt == 0)
	{
		read = 0;
		goto out;
	}

	if(!fd_may_access(f, FILE_ACCESS_READ))
	{
		errno = EBADF;
		goto error;
	}

	for(int i = 0; i < veccnt; i++)
	{
		struct iovec v;
		if(copy_from_user(&v, vec++, sizeof(struct iovec)) < 0)
		{
			errno = EFAULT;
			goto error;
		}
	
		if(v.iov_len == 0)
			continue;
		ssize_t was_read = read_vfs(offset, v.iov_len, v.iov_base, f);

		if(was_read < 0)
		{
			goto out;
		}

		read += was_read;
		offset += was_read;

		if((size_t) was_read != v.iov_len)
		{
			goto out;
		}
	}

out:
	fd_put(f);

	return read;
error:
	if(f)	fd_put(f);
	return -errno;
}

ssize_t sys_pwritev(int fd, const struct iovec *vec, int veccnt, off_t offset)
{
	size_t written = 0;

	struct file *f = get_file_description(fd);
	if(!f)
		goto error;

	if(!vec)
	{
		errno = EINVAL;
		goto error;
	}

	if(veccnt == 0)
	{
		written = 0;
		goto out;
	}

	if(!fd_may_access(f, FILE_ACCESS_WRITE))
	{
		errno = EBADF;
		goto error;
	}

	for(int i = 0; i < veccnt; i++)
	{
		struct iovec v;
		if(copy_from_user(&v, vec++, sizeof(struct iovec)) < 0)
		{
			errno = EFAULT;
			goto error;
		}
	
		if(v.iov_len == 0)
			continue;
		size_t was_written = write_vfs(offset,
			v.iov_len, v.iov_base,f);

		written += was_written;
		offset += was_written;

		if(was_written != v.iov_len)
		{
			goto out;
		}
	}

out:
	fd_put(f);

	return written;
error:
	if(f)	fd_put(f);
	return -errno;
}

unsigned int putdir(struct dirent *buf, struct dirent *ubuf, unsigned int count);

int sys_getdents(int fd, struct dirent *dirp, unsigned int count)
{
	int ret = 0;
	if(!count)
		return -EINVAL;

	struct file *f = get_file_description(fd);
	if(!f)
	{
		ret = -errno;
		goto out;
	}

	struct getdents_ret ret_buf = {0};
	ret = getdents_vfs(count, putdir, dirp, f->f_seek,
		&ret_buf, f);
	if(ret < 0)
	{
		ret = -errno;
		goto out;
	}

	f->f_seek = ret_buf.new_off;

	ret = ret_buf.read;
out:
	if(f)	fd_put(f);
	return ret;
}

int sys_ioctl(int fd, int request, char *argp)
{
	struct file *f = get_file_description(fd);
	if(!f)
	{
		return -errno;
	}

	int ret = ioctl_vfs(request, argp, f);

	fd_put(f);
	return ret;
}

int sys_truncate(const char *path, off_t length)
{
	return -ENOSYS;
}

int sys_ftruncate(int fd, off_t length)
{
	struct file *f = get_file_description(fd);
	if(!f)
	{
		return -errno;
	}

	int ret = 0;

	if(!fd_may_access(f, FILE_ACCESS_WRITE))
	{
		ret = -EBADF;
		goto out;
	}
	
	ret = ftruncate_vfs(length, f);

out:
	fd_put(f);
	return ret;
}

int sys_fallocate(int fd, int mode, off_t offset, off_t len)
{
	struct file *f = get_file_description(fd);
	if(!f)
	{
		return -errno;
	}

	int ret = fallocate_vfs(mode, offset, len, f);


	fd_put(f);
	return ret;
}

off_t sys_lseek(int fd, off_t offset, int whence)
{
	/* TODO: Fix O_APPEND behavior */
	off_t ret = 0;
	struct file *f = get_file_description(fd);
	if(!f)
		return -errno;

	/* TODO: Add a way for inodes to tell they don't support seeking */
	if(f->f_ino->i_type == VFS_TYPE_FIFO || f->f_ino->i_flags & INODE_FLAG_NO_SEEK)
	{
		ret = -ESPIPE;
		goto out;
	}
	
	if(whence == SEEK_CUR)
		ret = __sync_add_and_fetch(&f->f_seek, offset);
	else if(whence == SEEK_SET)
		ret = f->f_seek = offset;
	else if(whence == SEEK_END)
		ret = f->f_seek = f->f_ino->i_size + offset;
	else
	{
		ret = -EINVAL;
	}

out:
	fd_put(f);
	return ret;
}

int sys_mount(const char *usource, const char *utarget, const char *ufilesystemtype,
	      unsigned long mountflags, const void *data)
{
	const char *source = NULL;
	const char *target = NULL;
	struct file *block_file = NULL;
	const char *filesystemtype = NULL;
	int ret = 0;

	source = strcpy_from_user(usource);
	if(!source)
	{
		ret = -errno;
		goto out;
	}
	 
	target = strcpy_from_user(utarget);
	if(!target)
	{
		ret = -errno;
		goto out;
	}

	filesystemtype = strcpy_from_user(ufilesystemtype);
	if(!filesystemtype)
	{
		ret = -errno;
		goto out;
	}
	/* Find the 'filesystemtype's handler */
	filesystem_mount_t *fs = find_filesystem_handler(filesystemtype);
	if(!fs)
	{
		ret = -ENODEV;
		goto out;
	}

	block_file = open_vfs(get_fs_root(), source);
	if(!block_file)
	{
		ret = -ENOENT;
		goto out;
	}

	if(block_file->f_ino->i_type != VFS_TYPE_BLOCK_DEVICE)
	{
		ret = -ENOTBLK;
		goto out;
	}
	
	struct blockdev *d = blkdev_get_dev(block_file);
	struct inode *node = NULL;
	if(!(node = fs->handler(d)))
	{
		ret = -EINVAL;
		goto out;
	}

	char *str = strdup(target);
	mount_fs(node, str);
out:
	if(block_file) fd_put(block_file);
	if(source)   free((void *) source);
	if(target)   free((void *) target);
	if(filesystemtype) free((void *) filesystemtype);
	return ret;
}

int sys_pipe(int upipefd[2])
{
	int pipefd[2] = {-1, -1};
	int st = 0;

	/* Create the pipe */
	struct file *read_end, *write_end;

	if(pipe_create(&read_end, &write_end) < 0)
	{
		return -errno;
	}

	pipefd[0] = open_with_vnode(read_end, O_RDONLY);
	if(pipefd[0] < 0)
	{
		st = -errno;
		goto error;
	}

	pipefd[1] = open_with_vnode(write_end, O_WRONLY);
	if(pipefd[1] < 0)
	{
		st = -errno;
		goto error;
	}

	if(copy_to_user(upipefd, pipefd, sizeof(int) * 2) < 0)
	{
		st = -EFAULT;
		goto error;
	}

	fd_put(read_end);
	fd_put(write_end);

	return 0;
error:
	fd_put(read_end);
	fd_put(write_end);

	if(pipefd[0] != -1)
		file_close(pipefd[0]);
	if(pipefd[1] != -1)
		file_close(pipefd[1]);

	return -st;
}

int do_dupfd(struct file *f, int fdbase, bool cloexec)
{
	int new_fd = alloc_fd(fdbase);
	if(new_fd < 0)
		return new_fd;

	struct ioctx *ioctx = &get_current_process()->ctx;
	ioctx->file_desc[new_fd] = f;

	fd_get(f);

	fd_set_cloexec(new_fd, cloexec, ioctx);

	mutex_unlock(&ioctx->fdlock);

	return new_fd;
}

int fcntl_f_getfd(int fd, struct ioctx *ctx)
{
	mutex_lock(&ctx->fdlock);

	if(!validate_fd_number(fd, ctx))
	{
		mutex_unlock(&ctx->fdlock);
		return -EBADF;
	}

	int st = fd_is_cloexec(fd, ctx) ? FD_CLOEXEC : 0;

	mutex_unlock(&ctx->fdlock);
	return st;
}

int fcntl_f_setfd(int fd, unsigned long arg, struct ioctx *ctx)
{
	mutex_lock(&ctx->fdlock);

	if(!validate_fd_number(fd, ctx))
	{
		mutex_unlock(&ctx->fdlock);
		return -EBADF;
	}

	bool wants_cloexec = arg & FD_CLOEXEC;

	fd_set_cloexec(fd, wants_cloexec, ctx);

	mutex_unlock(&ctx->fdlock);

	return 0;
}

int fcntl_f_getfl(int fd, struct ioctx *ctx)
{
	bool is_cloexec;

	mutex_lock(&ctx->fdlock);

	if(!validate_fd_number(fd, ctx))
	{
		mutex_unlock(&ctx->fdlock);
		return -EBADF;
	}

	is_cloexec = fd_is_cloexec(fd, ctx);

	mutex_unlock(&ctx->fdlock);

	struct file *f = get_file_description(fd);
	if(!f)
		return -errno;
	unsigned int result = f->f_flags | (is_cloexec ? O_CLOEXEC : 0);

	fd_put(f);

	return result;
}

#define SETFL_MASK (O_APPEND | O_ASYNC | O_DIRECT | O_NOATIME | O_NONBLOCK)

int fcntl_f_setfl(int fd, struct ioctx *ctx, unsigned long arg)
{
	struct file *f = get_file_description(fd);
	if(!f)
		return -errno;
	
	/* TODO: Some flags, like O_ASYNC are not that simple to handle... */
	arg &= (O_APPEND | O_ASYNC | O_DIRECT | O_NOATIME | O_NONBLOCK);

	f->f_flags = arg | (f->f_flags & ~SETFL_MASK);

	fd_put(f);

	return 0;
}

int sys_fcntl(int fd, int cmd, unsigned long arg)
{
	/* TODO: Get new flags for file descriptors. The use of O_* is confusing since
	 * those only apply on open calls. For example, fcntl uses FD_*. */
	struct file *f = NULL;
	struct ioctx *ctx = &get_current_process()->ctx;

	int ret = 0;
	switch(cmd)
	{
		case F_DUPFD:
		{
			struct file *f = get_file_description(fd);
			if(!f)
				return -errno;

			ret = do_dupfd(f, (int) arg, false);
			break;
		}

		case F_DUPFD_CLOEXEC:
		{
			f = get_file_description(fd);
			if(!f)
				return -errno;

			ret = do_dupfd(f, (int) arg, true);
			break;
		}

		case F_GETFD:
		{
			return fcntl_f_getfd(fd, ctx);
		}

		case F_SETFD:
		{
			return fcntl_f_setfd(fd, arg, ctx);
		}

		case F_GETFL:
			return fcntl_f_getfl(fd, ctx);
		case F_SETFL:
			return fcntl_f_setfl(fd, ctx, arg);

		default:
			ret = -EINVAL;
			break;
	}

	if(f) fd_put(f);
	return ret;
}

#define STAT_FLAG_LSTAT          (1 << 0)

int do_sys_stat(const char *pathname, struct stat *buf, int flags, struct file *rel)
{
	unsigned int open_flags = (flags & STAT_FLAG_LSTAT ? OPEN_FLAG_NOFOLLOW : 0);
	struct file *base = get_fs_base(pathname, rel);
	struct file *stat_node = open_vfs_with_flags(base, pathname, open_flags);
	if(!stat_node)
		return -errno; /* Don't set errno, as we don't know if it was actually a ENOENT */

	int st = stat_vfs(buf, stat_node);
	fd_put(stat_node);
	return st < 0 ? -errno : st;
}

int sys_stat(const char *upathname, struct stat *ubuf)
{
	const char *pathname = strcpy_from_user(upathname);
	if(!pathname)
		return -errno;
	
	struct stat buf = {0};
	struct file *curr = get_current_directory();

	int st = do_sys_stat(pathname, &buf, 0, curr);

	fd_put(curr);

	if(copy_to_user(ubuf, &buf, sizeof(buf)) < 0)
	{
		st = -errno;
	}

	free((void *) pathname);
	return st;
}

int sys_lstat(const char *upathname, struct stat *ubuf)
{
	const char *pathname = strcpy_from_user(upathname);
	if(!pathname)
		return -errno;
	
	struct stat buf = {0};
	struct file *curr = get_current_directory();

	int st = do_sys_stat(pathname, &buf, STAT_FLAG_LSTAT, curr);

	fd_put(curr);

	if(copy_to_user(ubuf, &buf, sizeof(buf)) < 0)
	{
		st = -errno;
	}

	free((void *) pathname);
	return st;
}

int sys_fstat(int fd, struct stat *ubuf)
{
	int ret = 0;

	struct file *f = get_file_description(fd);
	if(!f)
	{
		ret = -errno;
		goto out;
	}

	struct stat buf = {0};

	if(stat_vfs(&buf, f) < 0)
	{
		ret = -errno;
		goto out;
	}

	if(copy_to_user(ubuf, &buf, sizeof(buf)) < 0)
	{
		ret = -EFAULT;
		goto out;
	}

out:
	if(f)	fd_put(f);
	return ret;
}

int sys_chdir(const char *upath)
{
	const char *path = strcpy_from_user(upath);
	if(!path)
		return -errno;

	int st = 0;
	struct file *curr = get_current_directory();
	struct file *base = get_fs_base(path, curr);
	struct file *dir = open_vfs(base, path);
	
	fd_put(curr);

	if(!dir)
	{
		st = -errno;
		goto out;
	}

	if(!(dir->f_ino->i_type & VFS_TYPE_DIR))
	{
		st = -ENOTDIR;
		goto close_file;
	}

	struct file *f = dir;

	struct process *current = get_current_process();
	struct ioctx *ctx = &current->ctx;
	spin_lock(&ctx->cwd_lock);

	struct file *old = ctx->cwd;
	ctx->cwd = f;

	spin_unlock(&ctx->cwd_lock);

	/* We've swapped ptrs atomically and now we're dropping the cwd reference.
	 * Note that any current users of the cwd are using it properly.
	*/
	fd_put(old);
	goto out;
close_file:
	if(dir)
		fd_put(dir);
out:
	if(path)	free((void *) path);
	return st;
}

int sys_fchdir(int fildes)
{
	struct file *f = get_file_description(fildes);
	if(!f)
		return -errno;

	struct file *node = f;
	if(!(node->f_ino->i_type & VFS_TYPE_DIR))
	{
		fd_put(f);
		return -ENOTDIR;
	}


	struct process *current = get_current_process();
	struct ioctx *ctx = &current->ctx;
	spin_lock(&ctx->cwd_lock);

	struct file *old = ctx->cwd;
	ctx->cwd = f;

	spin_unlock(&ctx->cwd_lock);

	/* We've swapped ptrs atomically and now we're dropping the cwd reference.
	 * Note that any current users of the cwd are using it properly.
	*/
	fd_put(old);

	return 0;
}

int sys_getcwd(char *path, size_t size)
{
	if(size == 0 && path != NULL)
		return -EINVAL;

	struct file *cwd = get_current_directory();
	char *name = dentry_to_file_name(cwd->f_dentry);

	fd_put(cwd);

	if(!name)
	{
		return -errno;
	}

	if(strlen(name) + 1 > size)
	{
		free(name);
		return -ERANGE;
	}

	if(copy_to_user(path, name, strlen(name) + 1) < 0)
	{
		free(name);
		return -errno;
	}

	return strlen(name);
}

struct file *get_dirfd_file(int dirfd)
{
	struct file *dirfd_desc = NULL;
	if(dirfd != AT_FDCWD)
	{
		dirfd_desc = get_file_description(dirfd);
		if(!dirfd_desc)
			return NULL;
	}
	else
		dirfd_desc = get_current_directory();

	return dirfd_desc;
}

int sys_openat(int dirfd, const char *upath, int flags, mode_t mode)
{
	struct file *dirfd_desc = NULL;

	dirfd_desc = get_dirfd_file(dirfd);
	if(!dirfd_desc)
		return -errno;

	const char *path = strcpy_from_user(upath);
	if(!path)
	{
		if(dirfd_desc) fd_put(dirfd_desc);
		return -errno;
	}

	int fd = do_sys_open(path, flags, mode, dirfd_desc);

	free((char *) path);
	if(dirfd_desc) fd_put(dirfd_desc);

	return fd;
}

int sys_fstatat(int dirfd, const char *upathname, struct stat *ubuf, int flags)
{
	const char *pathname = strcpy_from_user(upathname);
	if(!pathname)
		return -errno;
	struct stat buf = {0};
	struct file *dir;
	int st = 0;
	struct file *dirfd_desc = get_dirfd_file(dirfd);
	if(!dirfd_desc)
	{
		st = -errno;
		goto out;
	}

	dir = dirfd_desc;

	st = do_sys_stat(pathname, &buf, flags, dir);

	if(copy_to_user(ubuf, &buf, sizeof(buf)) < 0)
	{
		st = -errno;
		goto out;
	}
out:
	if(dirfd_desc)	fd_put(dirfd_desc);
	free((void *) pathname);
	return st;
}

int sys_fmount(int fd, const char *upath)
{
	struct file *f = get_file_description(fd);
	if(!f)
		return -errno;

	const char *path = strcpy_from_user(upath);
	if(!path)
	{
		fd_put(f);
		return -errno;
	}

	int st = mount_fs(f->f_ino, path);

	free((void *) path);
	fd_put(f);
	return st;
}

void file_do_cloexec(struct ioctx *ctx)
{
	mutex_lock(&ctx->fdlock);
	struct file **fd = ctx->file_desc;
	
	for(unsigned int i = 0; i < ctx->file_desc_entries; i++)
	{
		if(!fd[i])
			continue;
		if(fd_is_cloexec(i, ctx))
		{
			/* Close the file */
			__file_close_unlocked(i, get_current_process());
		}
	}

	mutex_unlock(&ctx->fdlock);
}

int open_with_vnode(struct file *node, int flags)
{
	/* This function does all the open() work, open(2) and openat(2) use this */
	struct ioctx *ioctx = &get_current_process()->ctx;

	int fd_num = -1;
	/* Allocate a file descriptor and a file description for the file */
	fd_num = file_alloc(node, ioctx);
	if(fd_num < 0)
	{
		mutex_unlock(&ioctx->fdlock);
		return -errno;
	}

	node->f_seek = 0;
	node->f_flags = flags;
	handle_open_flags(node, flags);
	bool cloexec = flags & O_CLOEXEC;
	fd_set_cloexec(fd_num, cloexec, ioctx);

	mutex_unlock(&ioctx->fdlock);
	return fd_num;
}

int sys_access(const char *path, int amode)
{
	int st = 0;
	char *p = strcpy_from_user(path);
	if(!p)
		return -errno;

	struct file *f = get_current_directory();

	struct file *ino = open_vfs(get_fs_base(p, f), p);
	fd_put(f);

	unsigned int mask = ((amode & R_OK) ? FILE_ACCESS_READ : 0) |
                        ((amode & X_OK) ? FILE_ACCESS_EXECUTE : 0) |
                        ((amode & W_OK) ? FILE_ACCESS_WRITE : 0);
	if(!ino)
	{
		st = -errno;
		goto out;
	}

	if(!file_can_access(ino, mask))
	{
		st = -EACCES;
		goto out;
	}
out:
	if(ino != NULL)	fd_put(ino);
	free(p);

	return st;
}

int do_sys_mkdir(const char *path, mode_t mode, struct file *dir)
{
	struct file *base = get_fs_base(path, dir);

	struct file *i = mkdir_vfs(path, mode & ~get_current_umask(), base->f_dentry);
	if(!i)
		return -errno;

	fd_put(i);
	return 0; 
}

int sys_mkdirat(int dirfd, const char *upath, mode_t mode)
{
	struct file *dir;
	struct file *dirfd_desc = NULL;

	dirfd_desc = get_dirfd_file(dirfd);
	if(!dirfd_desc)
	{
		return -errno;
	}

	dir = dirfd_desc;

	if(!(dir->f_ino->i_type & VFS_TYPE_DIR))
	{
		if(dirfd_desc) fd_put(dirfd_desc);
		return -ENOTDIR;
	}
	
	char *path = strcpy_from_user(upath);
	if(!path)
	{
		if(dirfd_desc) fd_put(dirfd_desc);
		return -errno;
	}

	int ret = do_sys_mkdir(path, mode, dir);

	free((char *) path);
	if(dirfd_desc) fd_put(dirfd_desc);

	return ret;
}

int sys_mkdir(const char *upath, mode_t mode)
{
	return sys_mkdirat(AT_FDCWD, upath, mode);
}

int do_sys_mknodat(const char *path, mode_t mode, dev_t dev, struct file *dir)
{
	struct file *base = get_fs_base(path, dir);

	struct file *i = mknod_vfs(path, mode & ~get_current_umask(), dev, base->f_dentry);
	if(!i)
		return -errno;

	fd_put(i);
	return 0; 
}

int sys_mknodat(int dirfd, const char *upath, mode_t mode, dev_t dev)
{
	struct file *dir;
	struct file *dirfd_desc = NULL;
	
	dirfd_desc = get_dirfd_file(dirfd);
	if(!dirfd_desc)
	{
		return -errno;
	}

	dir = dirfd_desc;

	if(!(dir->f_ino->i_type & VFS_TYPE_DIR))
	{
		if(dirfd_desc) fd_put(dirfd_desc);
		return -ENOTDIR;
	}
	
	char *path = strcpy_from_user(upath);
	if(!path)
	{
		if(dirfd_desc) fd_put(dirfd_desc);
		return -errno;
	}

	int ret = do_sys_mknodat(path, mode, dev, dir);

	free((char *) path);
	if(dirfd_desc) fd_put(dirfd_desc);

	return ret;
}

int sys_mknod(const char *pathname, mode_t mode, dev_t dev)
{
	return sys_mknodat(AT_FDCWD, pathname, mode, dev);
}

int do_sys_link(int olddirfd, const char *uoldpath, int newdirfd,
		const char *unewpath, int flags)
{
	/* TODO: Handle flags; same for every *at() syscall */
	int st = 0;
	char *oldpath = NULL;
	char *newpath = NULL;
	char *lname_buf = NULL;
	struct file *olddir = NULL;
	struct file *newdir = NULL;
	struct file *oldpathfile = NULL;
	struct file *newpathfile = NULL;
	oldpath = strcpy_from_user(uoldpath);
	newpath = strcpy_from_user(unewpath);

	if(!oldpath || !newpath)
	{
		st = -errno;
		goto out;
	}

	lname_buf = strdup(newpath);
	if(!lname_buf)
	{
		st = -errno;
		goto out;
	}


	olddir = get_dirfd_file(olddirfd);
	if(!olddir)
	{
		st = -errno;
		goto out;
	}

	newdir = get_dirfd_file(newdirfd);
	if(!newdir)
	{
		st = -errno;
		goto out;
	}

	oldpathfile = open_vfs(get_fs_base(oldpath, olddir), oldpath);
	if(!oldpathfile)
	{
		st = -errno;
		goto out;
	}

	char *to_open = dirname(newpath);

	newpathfile = open_vfs(get_fs_base(to_open, newdir), to_open);
	if(!newpathfile || newpathfile->f_ino->i_dev != oldpathfile->f_ino->i_dev)
	{
		/* Hard links need to be in the same filesystem */
		st = -EXDEV;
		goto out;
	}

	char *lname = basename(lname_buf);
	st = link_vfs(oldpathfile, lname, newpathfile);
out:
	if(lname_buf)	free(lname_buf);
	if(newpathfile)	fd_put(newpathfile);
	if(oldpathfile) fd_put(oldpathfile);
	if(oldpath)	free(oldpath);
	if(newpath)	free(newpath);
	if(olddir)	fd_put(olddir);
	if(newdir)	fd_put(newdir);
	return st;
}

int sys_link(const char *oldpath, const char *newpath)
{
	return do_sys_link(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);
}

int sys_linkat(int olddirfd, const char *oldpath,
               int newdirfd, const char *newpath, int flags)
{
	return do_sys_link(olddirfd, oldpath, newdirfd, newpath, flags);
}

int do_sys_unlink(int dirfd, const char *upathname, int flags)
{
	int st = 0;
	struct file *dirfd_file = NULL;
	char *buf = NULL;
	struct file *dir = NULL;
	char *pathname = strcpy_from_user(upathname);
	if(!pathname)
		return -errno;
	
	if(!(buf = strdup(pathname)))
	{
		goto out;
	}

	dirfd_file = get_dirfd_file(dirfd);
	if(!dirfd_file)
	{
		st = -errno;
		goto out;
	}

	char *to_open = dirname(pathname);
	dir = open_vfs(get_fs_base(to_open, dirfd_file), to_open);
	if(!dir)
	{
		st = -errno;
		goto out;
	}

	st = unlink_vfs(basename(buf), flags, dir);

out:
	if(dir)		fd_put(dir);
	if(buf)		free(buf);
	if(pathname)	free(pathname);
	if(dirfd_file)	fd_put(dirfd_file);

	return st;
}

int sys_unlink(const char *pathname)
{
	return do_sys_unlink(AT_FDCWD, pathname, 0);
}

int sys_unlinkat(int dirfd, const char *pathname, int flags)
{
	return do_sys_unlink(dirfd, pathname, flags);
}

int sys_rmdir(const char *pathname)
{
	/* Thankfully we can implement rmdir with unlinkat semantics 
	 * Thanks POSIX for this really nice and thoughtful API! */
	return do_sys_unlink(AT_FDCWD, pathname, AT_REMOVEDIR); 
}

ssize_t sys_readlinkat(int dirfd, const char *upathname, char *ubuf, size_t bufsiz)
{
	if((ssize_t) bufsiz < 0)
		return -EINVAL;
	ssize_t st = 0;
	char *pathname = strcpy_from_user(upathname);
	if(!pathname)
		return -errno;
	
	struct file *base = get_dirfd_file(dirfd);
	if(!base)
	{
		st = -errno;
		goto out;
	}

	struct file *f = open_vfs_with_flags(base, pathname, OPEN_FLAG_NOFOLLOW);
	if(!f)
	{
		st = -errno;
		goto out;
	}

	char *buf = readlink_vfs(f);
	if(!buf)
	{
		st = -errno;
		goto out1;
	}

	size_t buf_len = strlen(buf);
	size_t to_copy = buf_len < bufsiz ? buf_len : bufsiz;

	st = copy_to_user(ubuf, buf, to_copy);

	/* If the copy succeeded, set return to to_copy(it would be zero otherwise) */
	if(st == 0)
		st = to_copy;

	free(buf);
out1:
	fd_put(f);
out:
	free(pathname);
	if(base) fd_put(base);
	return st;
}

ssize_t sys_readlink(const char *pathname, char *buf, size_t bufsiz)
{
	return sys_readlinkat(AT_FDCWD, pathname, buf, bufsiz);
}

mode_t sys_umask(mode_t mask)
{
	struct process *current = get_current_process();
	mode_t old = current->ctx.umask;
	current->ctx.umask = mask & 0777;

	return old;
}

int sys_symlink(const char *target, const char *linkpath) {return -ENOSYS;}
int sys_symlinkat(const char *target, int newdirfd, const char *linkpath) {return -ENOSYS;}
int sys_chmod(const char *pathname, mode_t mode) {return -ENOSYS;}
int sys_fchmod(int fd, mode_t mode) {return -ENOSYS;}
int sys_fchmodat(int dirfd, const char *pathname, mode_t mode, int flags) {return -ENOSYS;}
int sys_chown(const char *pathname, uid_t owner, gid_t group) {return -ENOSYS;}
int sys_fchown(int fd, uid_t owner, gid_t group) {return -ENOSYS;}
int sys_lchown(const char *pathname, uid_t owner, gid_t group) {return -ENOSYS;}
int sys_fchownat(int dirfd, const char *pathname,
                    uid_t owner, gid_t group, int flags) {return -ENOSYS;}
int sys_rename(const char *oldpath, const char *newpath) {return -ENOSYS;}
int sys_renameat(int olddirfd, const char *oldpath,
                    int newdirfd, const char *newpath) {return -ENOSYS;}
int sys_utimensat(int dirfd, const char *pathname,
                     const struct timespec *times, int flags) {return -ENOSYS;}
int sys_faccessat(int dirfd, const char *pathname, int mode, int flags) {return -ENOSYS;}
