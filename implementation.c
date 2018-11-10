/* MyFS: a tiny file-system based on FUSE - Filesystem in Userspace

  Usage:
    gcc -Wall myfs.c implementation.c `pkg-config fuse --cflags --libs` -o myfs
    ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f

    May be mounted while running inside gdb (for debugging) with:
    gdb --args ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f

    It can then be unmounted (in another terminal) with
    fusermount -u ~/fuse-mnt
*/

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


/* Begin File System Documentatin -----------------------------------------
    
    File system structure is:

    To look up a file by a given absolute path:


/* End File System Documentatin ----------------------------------------- */

/* Begin Our Definitions -------------------------------------------------- */


#define FS_ROOTPATH ("/")                   // File system's root path
#define BLOCK_SZ_BYTES (4 * BYTES_IN_KB)    // Size of each MemBlock in the fs
#define FNAME_MAXLEN (256)                  // Max length of any filename
#define MAGIC_NUM (UINT32_C(0xdeadd0c5))    // Num for denoting block init

/* Inode header -
An Inode represents the meta-data of a file or directory. */
typedef struct Inode { 
    char fname[FNAME_MAXLEN];   // The file/folder's label
    int is_dir;                 // if == 1, node represents a dir, else a file
    int subdirs;                // Subdir count (unused unless is_dir == 1)
    size_t curr_size_bytes;     // Current file/dir size, in bytes
    size_t max_size_bytes;      // Max sz before needing more MemBlocks
    struct timespec last_acc;   // Last access time
    struct timespec last_mod;   // Last modified time
    void *memblock_offset;      // Offset from fsptr to file's 1st memblock
} Inode ;

/* Memory block header -
Each file and/or folder uses a number of memory blocks. Block size is
determined by BLOCK_SZ_BYTES. */
typedef struct MemBlock {
    size_t size_bytes;        // Mem block total size
    size_t user_size_bytes;   // Size of data field
    struct MemBlock *next;    // Offset to next MemBlock file/dir uses, if any
} MemBlock;

/* Top-level Filesystem "handle"
A file system is a list of inodes with each inode pointing to the offset of the 
first memory block used by the file/dir each inode represents. */
typedef struct FSHandle {
    uint32_t magic;                 // "Magic" number
    size_t size_bytes;              // Bytes available to fs (w/out this handle) 
    struct Inode *root_inode;       // Ptr to fs root dir's inode
    struct Inode *first_free;       // Ptr to the "free mem blocks" list
} FSHandle;

// Sizes of the above structs
#define INODE_OBJ_SZ sizeof(Inode)          // Size of Inode struct in bytes
#define MEMBLK_OBJ_SZ sizeof(MemBlock)      // Size of MemBlock struct in bytes
#define FSHANDLE_OBJ_SZ sizeof(FSHandle)    // Size of FSHandle struct in bytes

// Min requestable filesys size = FSHandle + 1 inode + 1 root dir + 1 memblock
#define MIN_FS_SZ_BYTES (FSHANDLE_OBJ_SZ + MEMBLK_OBJ_SZ) 


/* End Our Definitions ---------------------------------------------------- */
/* Begin Our Utility helpers ---------------------------------------------- */


/* Returns 1 iff fname is legal ascii chars and within max length, else 0. */
static int is_valid_filename(char *fname) {
    int len = 0;
    int ord = 0;

    for (char *c = fname; *c != '\0'; c++) {
        ord = (int) *c;
        if (ord < 48 || ord > 122)
            return 0;  // illegal ascii char found
        len++;
    }

    if (len && len <= FNAME_MAXLEN)
        return 1;
    return 0;
}


/* End Our Utility helpers ------------------------------------------------ */
/* Begin Our FS helpers --------------------------------------------------- */


// TODO: Inode* resolve_path(FSHandle *fs, const char *path) {
// TODO: void* get_file_data(FSHandle *fs, const char *path) {

/* Maps a file system of size fssize onto the given fsptr and returns *fs. */
static FSHandle* get_filesys(void *fsptr, size_t size) {
    if (size < MIN_FS_SZ_BYTES) return NULL;   // Validate size specified

    FSHandle *fs = (FSHandle*) fsptr;  // Map filesys onto given mem
    size_t fs_size = size - FSHANDLE_OBJ_SZ;

    // If memory hasn't been set up as a file system, do it now
    if (fs->magic != MAGIC_NUM) {
        memset(fsptr + FSHANDLE_OBJ_SZ, 0, fs_size);    // Zero-fill mem space

        // Populate file system data members
        fs->magic = MAGIC_NUM;
        fs->size_bytes = fs_size;

        fs->root_inode = (Inode*) (fsptr + FSHANDLE_OBJ_SZ);
        strncpy(FS_ROOTPATH, fs->root_inode->fname, 1);
        fs->root_inode->is_dir = 1;
        fs->root_inode->subdirs = 0;
        fs->root_inode->max_size_bytes = 0;
        fs->root_inode->curr_size_bytes = 0;
        // fs->root_inode->last_acc = NULL;        // TODO
        // fs->root_inode->last_mod = NULL;        // TODO
        fs->root_inode->memblock_offset = NULL; // TODO
        
        fs->first_free = NULL;                  // TODO

        // Fill the rest of the allocated space with mem block(s?) & add as free
        // MemBlock *memblock = (MemBlock*);
        // int memblock_offset = fsptr + FSHANDLE_OBJ_SZ + INODE_OBJ_SZ;
        // size_t grand_blocksz = MEMBLK_OBJ_SZ + BLOCK_SZ_BYTES;
        // size_t fs_boundry = fsptr + size;
        // while (memblock_offset < fs_boundry - BLOCK_SZ_BYTES) {
        // int num_blocks = fs_size / BLOCK_SZ_BYTES;
        // for (int i = 0; i < num_blocks; i++) {
        //     Inode *new_block = malloc(MEMBLK_OBJ_SZ);
        //     push_free_memblock(fs, i);
        // }
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
      FSHandle *fs_handle;
      Inode *inode;

      // Bind fs_handle to fs
      fs_handle = get_filesys(fsptr, fssize);
      if (!fs_handle) {
            *errnoptr = EFAULT;
            return -1;  // Fail, bad fsptr or fssize given
      }    

      // TODO: inode = path_resolve(fs_handle, path);
      inode = fs_handle->root_inode;
      if (!inode) {
            *errnoptr = ENOENT;
            return -1;  // Fail, bad path given
      }    
    
      //Reset the memory of the results container
      memset(stbuf, 0, sizeof(struct stat));   

      //Populate stdbuf based on the inode
      stbuf->st_uid = uid;
      stbuf->st_gid = gid;
      stbuf->st_atim = inode->last_acc; 
      stbuf->st_mtim = inode->last_mod;    
      
      if (inode->is_dir) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = inode->subdirs + 2;  // + 2 for . and ..
      } else {
            stbuf->st_mode = S_IFREG | 0755;
            stbuf->st_nlink = 1;
            stbuf->st_size = inode->curr_size_bytes;
      } 

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


/* Begin testing/debug  --------------------------------------------------- */
int main() 
{
    // printf("%d", is_valid_filename("test"));
    return 0;
}
