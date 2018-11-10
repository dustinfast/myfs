/* MyFS: a tiny file-system based on FUSE - Filesystem in Userspace

  Usage:
    gcc -Wall myfs.c implementation.c `pkg-config fuse --cflags --libs` -o myfs
    ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f

    May be mounted while running inside gdb (for debugging) with:
    gdb --args ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f

    It can then be unmounted (in another terminal) with
    fusermount -u ~/fuse-mnt
*/

// TODO: Remove un-needed includes
#include <stddef.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>


/* Helper types and functions */

// Helper const denoting if a mem chunk has been initialized to a fs or not.
#define MAGIC_NUM ((uint32_t) (UINT32_C(0xdeadd0c5)))

// Memory block structure - adapted from clauter's in-class lecture (DF):
struct __myfs_memory_block_t {  // MemBlock
      size_t      size;
      size_t      user_size;
      __myfs_off_t next;
} __myfs_memory_block_t;

// Filesystem handle structure - adapted from clauter's in-lass lecture (DF):
struct __myfs_handle_t {      // FSHandle
      uint32_t    magic;
      __myfs_off_t free_memory;
      __myfs_off_t root dir;
      size_t       size;
} __myfs_handle_t;

/* End of helper functions */


/* -- __myfs_getattr_implem() -- */
/* Implements the "stat" system call on the filesystem 

   Accepts:
      fsptr       : ptr to the fs
      fssize      : size of fs pointed to by fsptr
      errnoptr    : Error container
      uid         : User ID of file/dir owner
      gid         : Group ID of file/dir owner
      path        : Path of the file/dir in question
      stbuf       : Results container
   
   Returns:
      If path not a valid file or directory: returns -1  w/error in *errnoptr.
      If path is a valid file or directory: returns 0 with file/dir info
        copied to stbuf as -
            mode_t        File type and mode as fixed values:
                                    S_IFDIR | 0755 for directories,
                                    S_IFREG | 0755 for files
            nlink_t       Count of subdirectories (w/ . & ..), or just 1 if file)
            uid_t         Owners's user ID (from args)
            gid_t         Owner's group ID (from args)
            off_t         Real file size, in bytes (for files only)
            st_atim                  Last access time
            st_mtim                  Last modified time

   Example usage:
      struct fuse_context *context = fuse_get_context();
      struct __myfs_environment_struct_t *env;
      env = (struct __myfs_environment_struct_t *) (context->private_data);
      return THIS(env->memory, env->size, &err, env->uid, env->gid, path, st);
*/
int __myfs_getattr_implem(void *fsptr, size_t fssize, int *errnoptr,
                          uid_t uid, gid_t gid,
                          const char *path, struct stat *stbuf) {

    // The following was adapted from clauter's in-class lecture (DF):

    __myfs_handle_t handle;   // fs handle
    __myfs_inode_t *node;    // node ptr

      // Bind handle to fs
      handle = __myfs_get_handle(fsptr, fssize)
      if (!handle) {
            *errnoptr = EFAULT;
            return -1;  // Fail, bad filesystem given
      }

      // Associate the given path with a node in the fs
      node = __myfs_path_resolve(handle, path);
      if (!node) {
            *errnoptr = ENOENT;
            return -1;  // Fail, bad path given
      }

      // Reset the memory of the results container
      memset(stbuf, 0, sizeof(struct stat));

      // Populate results container based on the node
      stbuf->st_uid = uid;
      stbuf->st_gid = gid;
      stbuf->st_atim = node->last_access; 
      stbuf->st_mtim = node->last_mod;

      // TODO: node->value.directory...
      if (node->is_dir) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = node->subdirs + ((size_t) 2);
      } else {
            stbuf->st_mode = S_IFREG | 0755;
            stbuf->st_size = node->curr_size;
            stbuf->st_nlink = 1;
      }

      return 0;  // Success

  return -1;
}

/* Implements an emulation of the readdir system call on the filesystem 
   of size fssize pointed to by fsptr. 

   If path can be followed and describes a directory that exists and
   is accessable, the names of the subdirectories and files 
   contained in that directory are output into *namesptr. The . and ..
   directories must not be included in that listing.

   If it needs to output file and subdirectory names, the function
   starts by allocating (with calloc) an array of pointers to
   characters of the right size (n entries for n names). Sets
   *namesptr to that pointer. It then goes over all entries
   in that array and allocates, for each of them an array of
   characters of the right size (to hold the i-th name, together 
   with the appropriate '\0' terminator). It puts the pointer
   into that i-th array entry and fills the allocated array
   of characters with the appropriate name. The calling function
   will call free on each of the entries of *namesptr and 
   on *namesptr.

   The function returns the number of names that have been 
   put into namesptr. 

   If no name needs to be reported because the directory does
   not contain any file or subdirectory besides . and .., 0 is 
   returned and no allocation takes place.

   On failure, -1 is returned and the *errnoptr is set to 
   the appropriate error code. 

   The error codes are documented in man 2 readdir.

   In the case memory allocation with malloc/calloc fails, failure is
   indicated by returning -1 and setting *errnoptr to EINVAL.

*/
int __myfs_readdir_implem(void *fsptr, size_t fssize, int *errnoptr,
                          const char *path, char ***namesptr) {
  /* STUB */
  return -1;
}

/* Implements an emulation of the mknod system call for regular files
   on the filesystem of size fssize pointed to by fsptr.

   This function is called only for the creation of regular files.

   If a file gets created, it is of size zero and has default
   ownership and mode bits.

   The call creates the file indicated by path.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 mknod.

*/
int __myfs_mknod_implem(void *fsptr, size_t fssize, int *errnoptr,
                        const char *path) {
  /* STUB */
  return -1;
}

/* Implements an emulation of the unlink system call for regular files
   on the filesystem of size fssize pointed to by fsptr.

   This function is called only for the deletion of regular files.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 unlink.

*/
int __myfs_unlink_implem(void *fsptr, size_t fssize, int *errnoptr,
                        const char *path) {
  /* STUB */
  return -1;
}

/* Implements an emulation of the rmdir system call on the filesystem 
   of size fssize pointed to by fsptr. 

   The call deletes the directory indicated by path.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The function call must fail when the directory indicated by path is
   not empty (if there are files or subdirectories other than . and ..).

   The error codes are documented in man 2 rmdir.

*/
int __myfs_rmdir_implem(void *fsptr, size_t fssize, int *errnoptr,
                        const char *path) {
  /* STUB */
  return -1;
}

/* Implements an emulation of the mkdir system call on the filesystem 
   of size fssize pointed to by fsptr. 

   The call creates the directory indicated by path.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 mkdir.

*/
int __myfs_mkdir_implem(void *fsptr, size_t fssize, int *errnoptr,
                        const char *path) {
  /* STUB */
  return -1;
}

/* Implements an emulation of the rename system call on the filesystem 
   of size fssize pointed to by fsptr. 

   The call moves the file or directory indicated by from to to.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   Caution: the function does more than what is hinted to by its name.
   In cases the from and to paths differ, the file is moved out of 
   the from path and added to the to path.

   The error codes are documented in man 2 rename.

*/
int __myfs_rename_implem(void *fsptr, size_t fssize, int *errnoptr,
                         const char *from, const char *to) {
  /* STUB */
  return -1;
}

/* Implements an emulation of the truncate system call on the filesystem 
   of size fssize pointed to by fsptr. 

   The call changes the size of the file indicated by path to offset
   bytes.

   When the file becomes smaller due to the call, the extending bytes are
   removed. When it becomes larger, zeros are appended.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 truncate.

*/
int __myfs_truncate_implem(void *fsptr, size_t fssize, int *errnoptr,
                           const char *path, off_t offset) {
  /* STUB */
  return -1;
}

/* Implements an emulation of the open system call on the filesystem 
   of size fssize pointed to by fsptr, without actually performing the opening
   of the file (no file descriptor is returned).

   The call just checks if the file (or directory) indicated by path
   can be accessed, i.e. if the path can be followed to an existing
   object for which the access rights are granted.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The two only interesting error codes are 

   * EFAULT: the filesystem is in a bad state, we can't do anything

   * ENOENT: the file that we are supposed to open doesn't exist (or a
             subpath).

   It is possible to restrict ourselves to only these two error
   conditions. It is also possible to implement more detailed error
   condition answers.

   The error codes are documented in man 2 open.

*/
int __myfs_open_implem(void *fsptr, size_t fssize, int *errnoptr,
                       const char *path) {
  /* STUB */
  return -1;
}

/* Implements an emulation of the read system call on the filesystem 
   of size fssize pointed to by fsptr.

   The call copies up to size bytes from the file indicated by 
   path into the buffer, starting to read at offset. See the man page
   for read for the details when offset is beyond the end of the file etc.
   
   On success, the appropriate number of bytes read into the buffer is
   returned. The value zero is returned on an end-of-file condition.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 read.

*/
int __myfs_read_implem(void *fsptr, size_t fssize, int *errnoptr,
                       const char *path, char *buf, size_t size, off_t offset) {
  /* STUB */
  return -1;
}

/* Implements an emulation of the write system call on the filesystem 
   of size fssize pointed to by fsptr.

   The call copies up to size bytes to the file indicated by 
   path into the buffer, starting to write at offset. See the man page
   for write for the details when offset is beyond the end of the file etc.
   
   On success, the appropriate number of bytes written into the file is
   returned. The value zero is returned on an end-of-file condition.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 write.

*/
int __myfs_write_implem(void *fsptr, size_t fssize, int *errnoptr,
                        const char *path, const char *buf, size_t size, off_t offset) {
  /* STUB */
  return -1;
}

/* Implements an emulation of the utimensat system call on the filesystem 
   of size fssize pointed to by fsptr.

   The call changes the access and modification times of the file
   or directory indicated by path to the values in ts.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 utimensat.

*/
int __myfs_utimens_implem(void *fsptr, size_t fssize, int *errnoptr,
                          const char *path, const struct timespec ts[2]) {
  /* STUB */
  return -1;
}

/* Implements an emulation of the statfs system call on the filesystem 
   of size fssize pointed to by fsptr.

   The call gets information of the filesystem usage and puts in 
   into stbuf.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 statfs.

   Essentially, only the following fields of struct statvfs need to be
   supported:

   f_bsize   fill with what you call a block (typically 1024 bytes)
   f_blocks  fill with the total number of blocks in the filesystem
   f_bfree   fill with the free number of blocks in the filesystem
   f_bavail  fill with same value as f_bfree
   f_namemax fill with your maximum file/directory name, if your
             filesystem has such a maximum

*/
int __myfs_statfs_implem(void *fsptr, size_t fssize, int *errnoptr,
                         struct statvfs* stbuf) {
  /* STUB */
  return -1;
}
