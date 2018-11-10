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
#include "myfs_includes.h"

/* Begin Our Definitions -------------------------------------------------- */

#define FS_ROOTPATH ("/")                   // File system's root path
#define BLOCK_SZ_BYTES (1 * BYTES_IN_KB)    // Fs's mem block size in bytes
#define FNAME_MAXLEN (256)                  // Max length of any filename
#define PATH_MAXLEN (4096)                  // Max length of any path
#define MAGIC_NUM (UINT32_C(0xdeadd0c5))    // Num for denoting block init

/* Inode header -
An Inode is a file or directory header containings file/dir attributes. */
typedef struct Inode { 
    char filename[FNAME_MAXLEN];  // This file's (or dir's) label
    int is_dir;                   // 1 = node reprs a dir, else reprs a file
    int subdirs;                  // Count of subdirs (iff is_dir == 1)
    size_t max_size_bytes;        // Max size (before grow needed)
    size_t curr_size_bytes;       // Currently used space (of max_size)
    struct timespec last_access;  // Last access time
    struct timespec last_mod;     // Last modified time
    void *first_block;            // Ptr to first MemBlock used by the file/dir
} Inode ;

/* Memory block header -
Each file and/or folder uses a number of memory blocks. Block size is
determined by BLOCK_SZ_BYTES. */
typedef struct MemBlock {
    size_t size_bytes;        // Mem block size (BLOCK_SZ_BYTES aligned)
    size_t user_size_bytes;   // Size of data field
    struct MemBlock *next;    // Next mem block used by the file/dir, if any
} MemBlock;

/* Top-level Filesystem header
    A file system is a list of inodes with each node pointing to the memory 
    address used by the file/dir that inode represents. */
typedef struct FileSystem {
    uint32_t magic;                 // "Magic" number
    size_t size_bytes;              // Bytes available to fs (w/out header) 
    struct Inode *root_inode;       // Ptr to fs root dir's inode
    struct Inode *free_blocks;      // Ptr to the "free mem blocks" list
} FileSystem;

// Sizes of the above structs
#define INODE_OBJ_SZ sizeof(Inode)      // Size of Inode struct (bytes)
#define MEMBLK_OBJ_SZ sizeof(MemBlock)  // Size of MemBlock struct (bytes)
#define FS_OBJ_SZ sizeof(FileSystem)    // Size of FileSystem struct (bytes)
#define MIN_FS_SZ_BYTES (FS_OBJ_SZ + MEMBLK_OBJ_SZ) // Min requestable fs sz


/* End Our Definitions ---------------------------------------------------- */
/* Begin Our Utility helpers ---------------------------------------------- */


/* Given a filename, returns 1 if len(fname) within allowed length. */
static int is_valid_filename(char *fname) {
    size_t len = str_len(fname);
    if (len && len <= FNAME_MAXLEN)
        return 1;
    return 0;
}

/* Given a path, returns 1 if len(path) within allowed length. */
static int is_valid_path(char *path) {
    size_t len = str_len(path);
    if (len && len <= PATH_MAXLEN)
        return 1;
    return 0;
}


/* End Our Utility helpers ------------------------------------------------ */
/* Begin Our FS helpers --------------------------------------------------- */

//TODO: Inode* resolve_path(FileSystem handle, const char *path) {

/* Returns a file system ref from the given fsptr and fssize. */
static FileSystem* get_filesys(void *fsptr, size_t size) {
      if (size < MIN_FS_SZ_BYTES) return NULL;   // Validate size

      FileSystem *fs = (FileSystem*) fsptr;  // Map filesys onto given mem
      size_t fs_sz = size - FS_OBJ_SZ;       // Actual avail fs bytes

      // If memory hasn't been set up as a file system, do it now
      if (fs->magic != MAGIC_NUM) {
            memset(fsptr + FS_OBJ_SZ, 0, fs_sz);      // Zero-fill mem space

            // Populate file system fields
            fs->magic = MAGIC_NUM;
            fs->size_bytes = fs_sz;
            fs->root_inode = (Inode*) (FS_OBJ_SZ + fsptr);
            fs->free_blocks = NULL;

            // Chunk blocks
            // int num_blocks = fs_sz / BLOCK_SZ_BYTES;
            // TODO: add free blocks to free blocks list
            // TODO: Setup root inode
      } 

      return fs;   
}

/* End Our FS helpers ----------------------------------------------------- */
/* Begin Our 13 implementations ------------------------------------------- */


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
      -1  (w/error in *errnoptr) iff path not a valid file or directory. 
      Else returns 0 with file/dir info copied to stbuf as -
            nlink_t       Count of subdirectories (w/ . & ..), or just 1 if file)
            uid_t         Owners's user ID (from args)
            gid_t         Owner's group ID (from args)
            off_t         Real file size, in bytes (for files only)
            st_atim       Last access time
            st_mtim       Last modified time
            mode_t        File type/mode as S_IFDIR | 0755 for directories,
                                            S_IFREG | 0755 for files

   Example usage:
      struct fuse_context *context = fuse_get_context();
      struct __myfs_environment_struct_t *env;
      env = (struct __myfs_environment_struct_t *) (context->private_data);
      return THIS(env->memory, env->size, &err, env->uid, env->gid, path, st);
*/
int __myfs_getattr_implem(void *fsptr, size_t fssize, int *errnoptr,
                          uid_t uid, gid_t gid,
                          const char *path, struct stat *stbuf) {
      FileSystem *fs_handle;
      Inode *inode;

      // Bind fs_handle to fs
      fs_handle = get_filesys(fsptr, fssize);
      if (!fs_handle) {
            *errnoptr = EFAULT;
            return -1;  // Fail, bad filesystem given
      }    

      // TODO: inode = path_resolve(fs_handle, path);
      inode = fs_handle->root_inode;
      if (!inode) {
            *errnoptr = ENOENT;
            return -1;  // Fail, bad path given
      }    
    
      //Reset the memory of the results container
      memset(stbuf, 0, sizeof(struct stat));   

      // Populate stdbuf based on the inode
      // stbuf->st_uid = uid;
      // stbuf->st_gid = gid;
      // stbuf->st_atim = inode->last_access; 
      // stbuf->st_mtim = inode->last_mod;    
      
      // if (inode->is_dir) {
      //       stbuf->st_mode = S_IFDIR | 0755;
      //       stbuf->st_nlink = inode->subdirs + 2;  // + 2 for . and ..
      // } else {
      //       stbuf->st_mode = S_IFREG | 0755;
      //       stbuf->st_nlink = 1;
      //       stbuf->st_size = inode->curr_size_bytes;
      // } 

      return 0;  // Success  
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

/* End Our 13 implementations  -------------------------------------------- */

