/*

  MyFS: a tiny file-system written for educational purposes

  MyFS is 

  Copyright 2018 by

  University of Alaska Anchorage, College of Engineering.

  Contributor: Christoph Lauter

  and based on 

  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -g -O0 -Wall myfs.c implementation.c `pkg-config fuse --cflags --libs` -o myfs

  The filesystem can be mounted while it is running inside gdb (for
  debugging) purposes as follows (adapt to your setup):

  gdb --args ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f

  It can then be unmounted (in another terminal) with

  fusermount -u ~/fuse-mnt

  DO NOT CHANGE ANYTHING IN THIS FILE (UNLESS YOUR INSTRUCTOR ALLOWS
  YOU TO DO SO). 

  ALL YOUR CODE GOES INTO implementation.c !!!
  
*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <pthread.h>


struct __myfs_options_struct_t {
        const char *filename;
        const char *size;
        int show_help;
};

#define OPTION(t, p)  { t, offsetof(struct __myfs_options_struct_t, p), 1 }

static const struct fuse_opt __myfs_option_spec[] = {
        OPTION("--backupfile=%s", filename),
        OPTION("--size=%s", size),
        OPTION("-h", show_help),
        OPTION("--help", show_help),
        FUSE_OPT_END
};

struct __memory_block_struct_t {
  size_t size;
  size_t next;
};
typedef struct __memory_block_struct_t memory_block_t;

struct __myfs_environment_struct_t {
  pthread_mutex_t env_lock;
  uid_t           uid;
  gid_t           gid;
  void            *memory;
  size_t          size;
  int             using_backup;
  int             backup_fd;
};

#define MYFS_DEFAULT_SIZE  ((size_t) (128 << 20))   /* 128MB */
#define MYFS_MIN_SIZE      ((size_t) (2048))        /* 2kB */

static int __myfs_parse_size(size_t *size, const char *str) {
  unsigned long long int tmp, t;
  size_t s;
  char *end;

  if (*str == '\0') return 0;
  tmp = strtoull(str, &end, 0);
  if (*end != '\0') return 0;
  s = (size_t) tmp;
  t = (unsigned long long int) s;
  if (tmp != t) return 0;
  *size = s;
  return 1;
}

static int __myfs_setup_environment(struct __myfs_environment_struct_t *env, struct __myfs_options_struct_t *opts) {
  int size_specified, using_backup;
  size_t size;
  int fd;
  void *memory;
  off_t off;
  size_t len;
  size_t orig_size;

  /* Handle size */
  if (opts->size != NULL) {
    size_specified = 1;
    if (!__myfs_parse_size(&size, opts->size)) {
      fprintf(stderr, "Cannot parse size indication\n");
      return 0;
    }
    if (size < MYFS_MIN_SIZE) {
      size = MYFS_MIN_SIZE;
    }
  } else {
    size_specified = 0;
    size = MYFS_DEFAULT_SIZE;
  }
  
  /* Make sure size is at least the minimum size */
  if (size < MYFS_MIN_SIZE) {
    size = MYFS_MIN_SIZE;
  }

  /* Setup lock for the threads */
  if (pthread_mutex_init(&(env->env_lock), NULL) != 0) {
    perror("Cannot setup mutex");
    return 0;    
  }
  
  /* Handle backup file */
  if (opts->filename != NULL) {
    using_backup = 1;
    fd = open(opts->filename, O_CREAT | O_RDWR, 00644);
    if (fd < 0) {
      perror("Cannot open backup-file");
      if (pthread_mutex_destroy(&(env->env_lock)) != 0) {
        perror("Cannot destroy mutex");
      }
      return 0;
    }
    off = lseek(fd, 0, SEEK_END);
    if (off < ((off_t) 0)) {
      perror("Cannot seek in backup-file");
      if (pthread_mutex_destroy(&(env->env_lock)) != 0) {
        perror("Cannot destroy mutex");
      }
      return 0;
    }
    len = (size_t) off;
    orig_size = len;
    off = lseek(fd, 0, SEEK_SET);
    if (off < ((off_t) 0)) {
      perror("Cannot seek in backup-file");
      if (pthread_mutex_destroy(&(env->env_lock)) != 0) {
        perror("Cannot destroy mutex");
      }
      return 0;
    }
    if (size_specified) {
      if (len > size) {
        size = len;
      }
    } else {
      if (!(len == ((size_t) 0))) {
        size = len;
        if (size < MYFS_MIN_SIZE) {
          size = MYFS_MIN_SIZE;
        }
      } 
    }
    if (ftruncate(fd, size) != 0) {
      perror("Cannot seek in backup-file");
      if (pthread_mutex_destroy(&(env->env_lock)) != 0) {
        perror("Cannot destroy mutex");
      }
      return 0;
    }
  } else {
    using_backup = 0;
    fd = -1;
    orig_size = 0;
  }

  /* Do the mmap */
  if (using_backup) {
    memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED) {
      perror("Cannot map backup-file into memory");
      if (close(fd) != 0) {
        perror("Cannot close backup-file");
      }
      if (pthread_mutex_destroy(&(env->env_lock)) != 0) {
        perror("Cannot destroy mutex");
      }
      return 0;
    }
  } else {
    memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
      perror("Cannot map in memory");
      if (pthread_mutex_destroy(&(env->env_lock)) != 0) {
        perror("Cannot destroy mutex");
      }
      return 0;
    }
  }

  /* If the original size is different from the current size, we
     changed the filesystem and we need to wipe out the old filesystem
     completely.
  */
  if (using_backup) {
    if (orig_size != size) {
      if (orig_size != ((size_t) 0)) {
        memset(memory, 0, orig_size);
      }
    }
  }
  
  /* Get uid and gid, write back and succeed */
  env->uid = getuid();
  env->gid = getgid();
  env->memory = memory;
  env->size = size;
  env->using_backup = using_backup;
  env->backup_fd = fd;
  return 1;
}

static void __myfs_clear_environment(struct __myfs_environment_struct_t *env) {
  if (env->using_backup) {
    if (msync(env->memory, env->size, MS_SYNC) != 0) {
      perror("Cannot synchronize memory map with backup-file");
    }
  }
  if (munmap(env->memory, env->size) != 0) {
    perror("Cannot unmap memory");
  }
  if (env->using_backup) {
    if (close(env->backup_fd) != 0) {
      perror("Cannot close backup-file");
    }
  }
  if (pthread_mutex_destroy(&(env->env_lock)) != 0) {
    perror("Cannot destroy mutex");
  }
}

static int __myfs_sync_environment(struct __myfs_environment_struct_t *env) {
  if (env == NULL) return -1;
  if (!(env->using_backup)) return 0;
  if (msync(env->memory, env->size, MS_SYNC) != 0) return -1;
  if (fsync(env->backup_fd) != 0) return -1;
  return 0;
}

/* Declaration for the implementations of the operations */

int __myfs_getattr_implem(void *, size_t, int *, uid_t, gid_t, const char *, struct stat *);
int __myfs_readdir_implem(void *, size_t, int *, const char *, char ***);
int __myfs_mknod_implem(void *, size_t, int *, const char *);
int __myfs_unlink_implem(void *, size_t, int *, const char *);
int __myfs_mkdir_implem(void *, size_t, int *, const char *);
int __myfs_rmdir_implem(void *, size_t, int *, const char *);
int __myfs_rename_implem(void *, size_t, int *, const char *, const char*);
int __myfs_truncate_implem(void *, size_t, int *, const char *, off_t);
int __myfs_open_implem(void *, size_t, int *, const char *);
int __myfs_read_implem(void *, size_t, int *, const char *, char *, size_t, off_t);
int __myfs_write_implem(void *, size_t, int *, const char *, const char *, size_t, off_t);
int __myfs_statfs_implem(void *, size_t, int *, struct statvfs*);
int __myfs_utimens_implem(void *, size_t, int *, const char *, const struct timespec [2]);

/* End of declarations */

/* FUSE operations part */

static int __myfs_getattr(const char *path, struct stat *st) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;

  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);

  memset(st, 0, sizeof(struct stat));
  
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_getattr_implem(env->memory,
                              env->size,
                              &__myfs_errno,
                              env->uid,
                              env->gid,
                              path,
                              st);
  pthread_mutex_unlock(&(env->env_lock));  
  if (res >= 0)
    return res;
  return -__myfs_errno;
}

static int __myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                          off_t offset, struct fuse_file_info *fi) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res, i;
  char **names;
  
  (void) offset;
  (void) fi;
  
  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);

  names = NULL;
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_readdir_implem(env->memory,
                              env->size,
                              &__myfs_errno,
                              path,
                              &names);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0) {
    if (res == 0) {
      filler(buf, ".", NULL, 0);
      filler(buf, "..", NULL, 0);
      return 0;
    } else {
      if (names != NULL) {
        filler(buf, ".", NULL, 0);
        filler(buf, "..", NULL, 0);
        for (i=0;i<res;i++) {
          filler(buf, names[i], NULL, 0);
          free(names[i]);
        }
        free(names);
        return 0;
      } else {
        return -ENOENT;
      }
    }
  }
  return -__myfs_errno;
}

static int __myfs_mknod(const char* path, mode_t mode, dev_t dev) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;

  (void) dev;

  if (!S_ISREG(mode)) return -EPERM;
  
  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);
  
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_mknod_implem(env->memory,
                            env->size,
                            &__myfs_errno,
                            path);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0)
    return res;
  return -__myfs_errno;
}

static int __myfs_unlink(const char* path) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;
  
  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);
  
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_unlink_implem(env->memory,
                             env->size,
                             &__myfs_errno,
                             path);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0)
    return res;
  return -__myfs_errno;
}

static int __myfs_mkdir(const char* path, mode_t mode) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;
  
  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);
  
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_mkdir_implem(env->memory,
                            env->size,
                            &__myfs_errno,
                            path);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0)
    return res;
  return -__myfs_errno;
}

static int __myfs_rmdir(const char* path) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;

  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);
  
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_rmdir_implem(env->memory,
                            env->size,
                            &__myfs_errno,
                            path);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0)
    return res;
  return -__myfs_errno;
}

static int __myfs_rename(const char* from, const char* to) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;

  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);
  
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_rename_implem(env->memory,
                             env->size,
                             &__myfs_errno,
                             from,
                             to);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0)
    return res;
  return -__myfs_errno;
}

static int __myfs_truncate(const char* path, off_t size) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;

  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);
  
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_truncate_implem(env->memory,
                               env->size,
                               &__myfs_errno,
                               path,
                               size);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0)
    return res;
  return -__myfs_errno;
}

static int __myfs_open(const char* path, struct fuse_file_info* fi) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;

  if (!(((fi->flags & O_ACCMODE) == O_RDONLY) ||
        ((fi->flags & O_ACCMODE) == O_WRONLY) ||
        ((fi->flags & O_ACCMODE) == O_RDWR))) return -EINVAL;
  if (fi->flags & O_TRUNC) return -EINVAL;
  
  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);
  
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_open_implem(env->memory,
                           env->size,
                           &__myfs_errno,
                           path);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0)
    return res;
  return -__myfs_errno;
}

static int __myfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;

  (void) fi;
  
  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);
  
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_read_implem(env->memory,
                           env->size,
                           &__myfs_errno,
                           path,
                           buf,
                           size,
                           offset);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0)
    return res;
  return -__myfs_errno;
}

static int __myfs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;

  (void) fi;
  
  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);
  
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_write_implem(env->memory,
                            env->size,
                            &__myfs_errno,
                            path,
                            buf,
                            size,
                            offset);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0)
    return res;
  return -__myfs_errno;
}

static int __myfs_statfs(const char* path, struct statvfs* stbuf) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;

  (void) path;
  
  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);

  memset(stbuf, 0, sizeof(struct statvfs));
  
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_statfs_implem(env->memory,
                             env->size,
                             &__myfs_errno,
                             stbuf);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0)
    return res;
  return -__myfs_errno;
}

static int __myfs_utimens(const char* path, const struct timespec ts[2]) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;

  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);
  
  __myfs_errno = ENOENT;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_utimens_implem(env->memory,
                              env->size,
                              &__myfs_errno,
                              path,
                              ts);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0)
    return res;
  return -__myfs_errno;
}

static int __myfs_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
  struct fuse_context *context;
  struct __myfs_environment_struct_t *env;
  int __myfs_errno, res;
  
  (void) path;
  (void) datasync;
  (void) fi;

  context = fuse_get_context();
  env = (struct __myfs_environment_struct_t *) (context->private_data);
  
  __myfs_errno = EIO;
  pthread_mutex_lock(&(env->env_lock));
  res = __myfs_sync_environment(env);
  pthread_mutex_unlock(&(env->env_lock));
  if (res >= 0)
    return res;
  return -__myfs_errno;  
}

static void __myfs_destroy(void *private_data) {
  struct __myfs_environment_struct_t *env;
  
  if (private_data == NULL) return;
  env = (struct __myfs_environment_struct_t *) private_data;
  __myfs_clear_environment(env);
}

static struct fuse_operations __myfs_operations = {
  .getattr = __myfs_getattr,
  .readdir = __myfs_readdir,
  .mkdir = __myfs_mkdir,
  .mknod = __myfs_mknod,
  .unlink = __myfs_unlink,
  .rmdir = __myfs_rmdir,
  .rename = __myfs_rename,
  .truncate = __myfs_truncate,
  .open = __myfs_open,
  .read = __myfs_read,
  .write = __myfs_write,
  .statfs = __myfs_statfs,
  .utimens = __myfs_utimens,
  .fsync = __myfs_fsync,
  .destroy = __myfs_destroy
};

/* End of FUSE operations part */

static void __myfs_show_help(const char *name) {
        printf("usage: %s [options] <mountpoint>\n\n", name);
        printf("File-system specific options:\n"
               "    --backupfile=<s>        File to read file-system content from and save to\n"
               "                            Default: none, all changes are lost\n"
               "    --size=<s>              Size of the file system\n"
               "                            Default: 128MB if no backup-file is given.\n"
               "                                     Size of the backup-file otherwise.\n"
               "                            If both a backup-file and a size are specified,\n"
               "                            the actual size is the maximum of the size of the\n"
               "                            backup-file and the size specified.\n"
               "                            The minimum size of a filesystem is 2kB. If a\n"
               "                            lesser size is used, it is increased to 2kB.\n"
               "\n");
}

int main(int argc, char *argv[]) {
  struct __myfs_options_struct_t __myfs_options;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct __myfs_environment_struct_t __myfs_environment;
  struct __myfs_environment_struct_t *env_ptr = NULL;
  
  /* Initialize defaults */
  __myfs_options.filename = NULL;
  __myfs_options.size = NULL;
  __myfs_options.show_help = 0;
        
  /* Parse options */
  if (fuse_opt_parse(&args, &__myfs_options, __myfs_option_spec, NULL) == -1)
    return 1;

  /* If we are not just handling help texts, we need to setup the
     file-system environment.
  */
  if (!__myfs_options.show_help) {
    env_ptr = &__myfs_environment;
    if (!__myfs_setup_environment(env_ptr, &__myfs_options))
      return 1;
  } else {
    /* Handle displaying of help text */
    __myfs_show_help(argv[0]);
    assert(fuse_opt_add_arg(&args, "--help") == 0);
    args.argv[0] = (char*) "";
  }
  
  return fuse_main(args.argc, args.argv, &__myfs_operations, env_ptr);
}
