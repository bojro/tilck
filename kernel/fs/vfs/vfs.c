/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/string_util.h>

#include <tilck/kernel/fs/vfs.h>
#include <tilck/kernel/kmalloc.h>
#include <tilck/kernel/errno.h>
#include <tilck/kernel/process.h>
#include <tilck/kernel/user.h>

#include <dirent.h> // system header

#include "../fs_int.h"
#include "vfs_locking.c.h"
#include "vfs_resolve.c.h"
#include "vfs_getdents.c.h"
#include "vfs_op_ready.c.h"

static u32 next_device_id;

static void __nr_check(bool *check)
{
   if (!*check)
      panic("Detected return in VFS func using common header/footer");
}

#define VFS_FS_PATH_FUNCS_COMMON_HEADER(path_param, exlock, rl)         \
                                                                        \
   char lc[MAX_PATH]; /* last comp */                                   \
   filesystem *fs;                                                      \
   vfs_path p;                                                          \
   int rc;                                                              \
   DEBUG_ONLY(const bool __saved_exlock = exlock);                      \
   DEBUG_ONLY(bool no_ret_check __attribute__((cleanup(__nr_check))));  \
   DEBUG_ONLY(no_ret_check = false);                                    \
                                                                        \
   NO_TEST_ASSERT(is_preemption_enabled());                             \
   ASSERT(path_param != NULL);                                          \
                                                                        \
   if ((rc = vfs_resolve(path_param, &p, lc, exlock, rl)) < 0) {        \
      DEBUG_ONLY(no_ret_check = true);                                  \
      return rc;                                                        \
   }                                                                    \
                                                                        \
   ASSERT(p.fs != NULL);                                                \
   fs = p.fs;

#define VFS_FS_PATH_FUNCS_COMMON_FOOTER(path_param, exlock, rl)         \
out:                                                                    \
   DEBUG_ONLY(no_ret_check = true);                                     \
   ASSERT(exlock == __saved_exlock);                                    \
   exlock ? vfs_fs_exunlock(fs) : vfs_fs_shunlock(fs);                  \
   release_obj(fs);                                                     \
   return rc;

/* ------------ handle-based functions ------------- */

void vfs_close(fs_handle h)
{
   /*
    * TODO: consider forcing also vfs_close() to be run always with preemption
    * enabled. Reason: when one day when actual I/O devices will be supported,
    * close() might need in some cases to do some I/O.
    *
    * What prevents vfs_close() to run with preemption enabled is the function
    * terminate_process() which requires disabled preemption, because of its
    * (primitive) sync with signal handling.
    */
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;
   filesystem *fs = hb->fs;

#ifndef UNIT_TEST_ENVIRONMENT
   process_info *pi = get_curr_task()->pi;
   remove_all_mappings_of_handle(pi, h);
#endif

   fs->fsops->close(h);
   release_obj(fs);

   /* while a filesystem is mounted, the minimum ref-count it can have is 1 */
   ASSERT(get_ref_count(fs) > 0);
}

int vfs_dup(fs_handle h, fs_handle *dup_h)
{
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;
   int rc;

   if (!hb)
      return -EBADF;

   if ((rc = hb->fs->fsops->dup(h, dup_h)))
      return rc;

   /* The new file descriptor does NOT share old file descriptor's fd_flags */
   ((fs_handle_base*) *dup_h)->fd_flags = 0;

   retain_obj(hb->fs);
   ASSERT(*dup_h != NULL);
   return 0;
}

ssize_t vfs_read(fs_handle h, void *buf, size_t buf_size)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;
   ssize_t ret;

   if (!hb->fops->read)
      return -EBADF;

   if ((hb->fl_flags & O_WRONLY) && !(hb->fl_flags & O_RDWR))
      return -EBADF; /* file not opened for reading */

   vfs_shlock(h);
   {
      ret = hb->fops->read(h, buf, buf_size);
   }
   vfs_shunlock(h);
   return ret;
}

ssize_t vfs_write(fs_handle h, void *buf, size_t buf_size)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;
   ssize_t ret;

   if (!hb->fops->write)
      return -EBADF;

   if (!(hb->fl_flags & (O_WRONLY | O_RDWR)))
      return -EBADF; /* file not opened for writing */

   vfs_exlock(h);
   {
      ret = hb->fops->write(h, buf, buf_size);
   }
   vfs_exunlock(h);
   return ret;
}

off_t vfs_seek(fs_handle h, s64 off, int whence)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);
   off_t ret;

   if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
      return -EINVAL; /* Tilck does NOT support SEEK_DATA and SEEK_HOLE */

   fs_handle_base *hb = (fs_handle_base *) h;

   if (!hb->fops->seek)
      return -ESPIPE;

   vfs_shlock(h);
   {
      // NOTE: this won't really work for big offsets in case off_t is 32-bit.
      ret = hb->fops->seek(h, (off_t) off, whence);
   }
   vfs_shunlock(h);
   return ret;
}

int vfs_ioctl(fs_handle h, uptr request, void *argp)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;
   int ret;

   if (!hb->fops->ioctl)
      return -ENOTTY; // Yes, ENOTTY *IS* the right error. See the man page.

   vfs_exlock(h);
   {
      ret = hb->fops->ioctl(h, request, argp);
   }
   vfs_exunlock(h);
   return ret;
}

int vfs_fcntl(fs_handle h, int cmd, int arg)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   fs_handle_base *hb = (fs_handle_base *) h;
   int ret;

   if (!hb->fops->fcntl)
      return -EINVAL;

   vfs_exlock(h);
   {
      ret = hb->fops->fcntl(h, cmd, arg);
   }
   vfs_exunlock(h);
   return ret;
}

int vfs_ftruncate(fs_handle h, off_t length)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   const fs_ops *fsops = hb->fs->fsops;

   if (!fsops->truncate)
      return -EROFS;

   return fsops->truncate(hb->fs, fsops->get_inode(h), length);
}

int vfs_fstat64(fs_handle h, struct stat64 *statbuf)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;
   filesystem *fs = hb->fs;
   const fs_ops *fsops = fs->fsops;
   int ret;

   vfs_shlock(h);
   {
      ret = fsops->stat(fs, fsops->get_inode(h), statbuf);
   }
   vfs_shunlock(h);
   return ret;
}

/* ----------- path-based functions -------------- */

int vfs_open(const char *path, fs_handle *out, int flags, mode_t mode)
{
   if (flags & O_ASYNC)
      return -EINVAL; /* TODO: Tilck does not support ASYNC I/O yet */

   if ((flags & O_TMPFILE) == O_TMPFILE)
      return -EOPNOTSUPP; /* TODO: Tilck does not support O_TMPFILE yet */

   VFS_FS_PATH_FUNCS_COMMON_HEADER(path, true, true)

   if (!(rc = fs->fsops->open(&p, out, flags, mode))) {

      /* open() succeeded, the FS is already retained */
      ((fs_handle_base *) *out)->fl_flags = flags;

      if (flags & O_CLOEXEC)
         ((fs_handle_base *) *out)->fd_flags |= FD_CLOEXEC;

      /* file handles retain their filesystem */
      retain_obj(fs);
   }

   VFS_FS_PATH_FUNCS_COMMON_FOOTER(path, true, true)
}

int vfs_stat64(const char *path, struct stat64 *statbuf, bool res_last_sl)
{
   VFS_FS_PATH_FUNCS_COMMON_HEADER(path, false, res_last_sl)

   rc = p.fs_path.inode
      ? fs->fsops->stat(fs, p.fs_path.inode, statbuf)
      : -ENOENT;

   VFS_FS_PATH_FUNCS_COMMON_FOOTER(path, false, res_last_sl)
}

int vfs_mkdir(const char *path, mode_t mode)
{
   VFS_FS_PATH_FUNCS_COMMON_HEADER(path, true, false)

   if (fs->fsops->mkdir) {
      if (fs->flags & VFS_FS_RW) {
         rc = p.fs_path.inode
            ? -EEXIST
            :  fs->fsops->mkdir(&p, mode);
      } else {
         rc = -EROFS;
      }
   } else {
      rc = -EPERM;
   }

   VFS_FS_PATH_FUNCS_COMMON_FOOTER(path, true, false)
}

int vfs_rmdir(const char *path)
{
   VFS_FS_PATH_FUNCS_COMMON_HEADER(path, true, false)

   if (fs->fsops->rmdir) {
      if (fs->flags & VFS_FS_RW) {
         rc = p.fs_path.inode
            ? fs->fsops->rmdir(&p)
            : -ENOENT;
      } else {
         rc = -EROFS;
      }
   } else {
      rc = -EPERM;
   }

   VFS_FS_PATH_FUNCS_COMMON_FOOTER(path, true, false)
}

int vfs_unlink(const char *path)
{
   VFS_FS_PATH_FUNCS_COMMON_HEADER(path, true, false)

   if (fs->fsops->unlink) {
      if (fs->flags & VFS_FS_RW) {
         rc = p.fs_path.inode
            ? fs->fsops->unlink(&p)
            : -ENOENT;
      } else {
         rc = -EROFS;
      }
   } else {
      rc = -EROFS;
   }

   VFS_FS_PATH_FUNCS_COMMON_FOOTER(path, true, false)
}

int vfs_truncate(const char *path, off_t len)
{
   VFS_FS_PATH_FUNCS_COMMON_HEADER(path, false, true)

   if (fs->fsops->truncate) {

      if (fs->flags & VFS_FS_RW) {
         rc = p.fs_path.inode
            ? fs->fsops->truncate(fs, p.fs_path.inode, len)
            : -ENOENT;
      } else {
         rc = -EROFS;
      }

   } else {
      rc = -EROFS;
   }

   VFS_FS_PATH_FUNCS_COMMON_FOOTER(path, false, true)
}

int vfs_symlink(const char *target, const char *linkpath)
{
   VFS_FS_PATH_FUNCS_COMMON_HEADER(linkpath, true, false)

   if (fs->fsops->symlink) {

      if (fs->flags & VFS_FS_RW) {
         rc = p.fs_path.inode
            ? -EEXIST /* the linkpath already exists! */
            : fs->fsops->symlink(target, &p);
      } else {
         rc = -EROFS;
      }

   } else {
      rc = -EPERM; /* symlinks not supported */
   }

   VFS_FS_PATH_FUNCS_COMMON_FOOTER(path, true, false)
}

/* NOTE: `buf` is guaranteed to have room for at least MAX_PATH chars */
int vfs_readlink(const char *path, char *buf)
{
   VFS_FS_PATH_FUNCS_COMMON_HEADER(path, false, false)

   if (fs->fsops->readlink) {

      /* there is a readlink function */

      rc = p.fs_path.inode
         ? fs->fsops->readlink(&p, buf)
         : -ENOENT;

   } else {

      /*
       * If there's no readlink(), symlinks are not supported by the FS, ergo
       * the last component of `path` cannot be referring to a symlink.
       */
      rc = -EINVAL;
   }

   VFS_FS_PATH_FUNCS_COMMON_FOOTER(path, false, false)
}

u32 vfs_get_new_device_id(void)
{
   return next_device_id++;
}
