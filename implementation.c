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


/* Begin File System Documentati0n ---------------------------------------- /*
    
    + File system structure in memory is:
     _ _ _ _ _ _ _ _ _______________________ ___________________________
    |   FSHandle    |       Inodes          |       Memory Blocks       | 
    |_ _ _ _ _ _ _ _|_______________________|___________________________|
    ^               ^                       ^
    fsptr           Inodes Segment          Memory Blocks segment
                    (0th is root inode)     (0th is root dir memory block)

    + Memory blocks look like:
     ______________ ________________________
    |   MemHead    |         Data           |
    |______________|________________________|


/* End File System Documentation ------------------------------------------ */
/* Begin Our Definitions -------------------------------------------------- */

#define FS_ROOTPATH ("/\0")                 // File system's root path
#define FS_BLOCK_SZ_KB (1)                  // Total kbs of each memory block
#define FNAME_MAXLEN (256)                  // Max length of any filename
#define MAGIC_NUM (UINT32_C(0xdeadd0c5))    // Num for denoting block init

// Inode -
// An Inode represents the meta-data of a file or folder.
typedef struct Inode { 
    char fname[FNAME_MAXLEN];   // The file/folder's label
    int *is_dir;                // if 1, node represents a dir, else a file
    int *subdirs;               // Subdir count (unused if not is_dir)
    size_t *file_size_b;        // File's/folder's data size, in bytes
    struct timespec *last_acc;  // File/folder last access time
    struct timespec *last_mod;  // File/Folder last modified time
    int *offset_firstblk;       // Byte offset from fsptr to file/folder's 1st
                                // memblock, or -1 if inode is free/unused
} Inode;

// Memory block header -
// Each file/dir uses one or more memory blocks.
typedef struct MemHead {
    size_t *data_size_b;    // Size of data field occupied, or 0 if block free
    int *offset_nextblk;    // Bytes offset (from fsptr) to next block of 
                            // file's data, if any. Else -1.
                            // (data field immediately follows this field)
} MemHead;

// Top-level filesystem handle
// A file system is a list of inodes where each knows the offset of the first 
// memory block for that file/dir.
typedef struct FSHandle {
    uint32_t magic;             // "Magic" number, for denoting mem ini'd
    size_t size_b;              // Fs sz from inode seg to end of mem blocks
    struct Inode *inode_seg;    // Ptr to start of inodes segment
    struct MemHead *mem_seg;    // Ptr to start of mem blocks segment
} FSHandle;

// Size in bytes of the filesystem's structs (above)
#define ST_SZ_INODE sizeof(Inode)
#define ST_SZ_MEMHEAD sizeof(MemHead)
#define ST_SZ_FSHANDLE sizeof(FSHandle)  

// Size of each memory block's data field (after kb aligning w/header) 
#define DATAFIELD_SZ_B (FS_BLOCK_SZ_KB * BYTES_IN_KB - sizeof(MemHead))    

// Memory block size = MemHead + data field of size DATAFIELD_SZ_B
#define MEMBLOCK_SZ_B sizeof(MemHead) + DATAFIELD_SZ_B

// Min requestable fs size = FSHandle + 1 inode + root dir block + 1 free block
#define MIN_FS_SZ_B sizeof(FSHandle) + sizeof(Inode) + (2 * MEMBLOCK_SZ_B) 

// Offset in bytes from fsptr to start of inodes segment
#define FS_START_OFFSET sizeof(FSHandle)


/* End Our Definitions ---------------------------------------------------- */
/* Begin Our Utility helpers ---------------------------------------------- */


// Returns a ptr to a mem address in the file system given an offset.
static void* ptr_from_offset(FSHandle *fs, int *offset) {
    return (void*)((long unsigned int)fs + (size_t)offset);
}

// Returns an int offset from the filesystem's start address for the given ptr.
static int offset_from_ptr(FSHandle *fs, void *ptr) {
    return ptr - (void*)fs;
}

// Returns 1 iff fname is legal ascii chars and within max length, else 0.
static int is_filename_valid(char *fname) {
    int len = 0;
    int ord = 0;

    for (char *c = fname; *c != '\0'; c++) {
        ord = (int) *c;
        if (ord < 32 || ord == 44 || ord  == 47 || ord > 122)
            return 0;  // illegal ascii char found
        len++;
    }

    if (len && len <= FNAME_MAXLEN)
        return 1;
    return 0;
}

// Sets the last access time for the given node to the current time.
// If set_modified, also sets the last modified time to the current time.
static void set_inode_lasttimes(Inode *inode, int set_modified) {
    if (!inode) return;  // Validate node

    struct timespec tspec;
    clock_gettime(CLOCK_REALTIME, &tspec);

    inode->last_acc = &tspec;
    if (set_modified)
        inode->last_mod = &tspec;
}

// Returns 1 if the given node is free, else returns 0.
static int is_inode_free(Inode *inode) {
    if (inode->offset_firstblk == (int*) -1)
        return 1;
    return 0;
}

// Returns 1 if the given memory block is free, else returns 0.
static int is_memblock_free(MemHead *memhead) {
    if (memhead->data_size_b == 0)
        return 1;
    return 0;
}

/* End Our Utility helpers ------------------------------------------------ */
/* Begin Our FS helpers --------------------------------------------------- */


// TODO: Inode* resolve_path(FSHandle *fs, const char *path) {
// TODO: void* get_file_data(FSHandle *fs, const char *path) {

// Sets the data field and updates size fields for file denoted by given inode.
static int set_filedata(FSHandle *fs, Inode *inode, char *data, size_t sz) {
    MemHead *memblock = ptr_from_offset(fs, inode->offset_firstblk);
    void *data_field = memblock + ST_SZ_MEMHEAD;

    // Use a single block if sz will fit in one
    if (sz <= DATAFIELD_SZ_B) {
        strncpy(data_field, data, sz);
        memblock->data_size_b = (size_t*) sz;
        memblock->offset_nextblk = NULL;         // Only single block used
    }

    // Else use multiple blocks, if available
    else {
        int blocks_needed = sz / DATAFIELD_SZ_B + 1;
        printf("UNSUPORTED: need %d blocks for this operation.", blocks_needed);
        return 0;
    }

    // TODO: set_inode_lasttimes(inode, 1) -- set_inode works from get_filesys, but not from here
    inode->file_size_b = (size_t*) sz;

    return 1;
}

// Formats file system's memory space and initializes ptr fs.
void init_filesys_mem(void *fsptr, size_t size, FSHandle *fs) {
    if (size < MIN_FS_SZ_B) return; // Ensure adequate size given

    size_t fs_size = size - FS_START_OFFSET;    // Space available to fs
    void *segs_start = fsptr + FS_START_OFFSET; // Start of fs segments
    void *root_dir_start = NULL;                // Mem block segment start addr
    size_t rootdir_offset = -1;                 // Root dir offset from fsptr

    memset(fsptr, 0, fs_size);                  // Format mem space w/zero-fill

    // Determine num inodes & memblocks fs_size will allow (1:1 ratio)
    int n_blocks = 0;
    int n_inodes = 0;
    while (n_blocks * DATAFIELD_SZ_B + n_inodes * ST_SZ_INODE < fs_size) {
        n_blocks++;
        n_inodes++;
    }
    
    // Denote root dir and first free addresses abd ffsets
    root_dir_start = segs_start + (ST_SZ_INODE * n_inodes);
    rootdir_offset = root_dir_start - fsptr;

    // debug
    printf("    ** New memspace detected - formatting as new file system...\n");
    printf("    Inodes                      : %d\n",n_inodes);
    printf("    Memory Blocks               : %d\n",n_blocks);
    printf("    Inodes segment start        : %lu\n", (long unsigned int)segs_start);
    printf("    Mem blocks segment start    : %lu\n", (long unsigned int)root_dir_start);
    printf("    Rootdir block offset        : %lu\n", (long unsigned int)rootdir_offset);

    // Populate fs data members
    fs->magic = MAGIC_NUM;
    fs->size_b = fs_size;
    fs->inode_seg = (Inode*) segs_start;
    fs->mem_seg = (MemHead*) root_dir_start;

    // Set up 0th inode as the root inode
    strncpy(fs->inode_seg->fname, FS_ROOTPATH, str_len(FS_ROOTPATH));
    set_inode_lasttimes(fs->inode_seg, 1);
    fs->inode_seg->is_dir = (int*) 1;
    fs->inode_seg->subdirs = 0;
    fs->inode_seg->offset_firstblk = (int*) rootdir_offset;
    
    // Set up 0th memory block as the root directory
    // TODO: Write root dir table data w/. and ..
    set_filedata(fs, fs->inode_seg, "test\0", 5);
}

// Maps a filesystem of size fssize onto fsptr and returns a handle to it.
static FSHandle* get_filesys_handle(void *fsptr, size_t size) {
    // Map file system structure onto the given memory space
    FSHandle *fs = (FSHandle*) fsptr;

    // If the first few bytes aren't our magic number, the memory hasn't yet
    // been initialized as a file system, so do it now.
    if (fs->magic != MAGIC_NUM)
        init_filesys_mem(fsptr, size, fs);

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
      fs_handle = get_filesys_handle(fsptr, fssize);
      if (!fs_handle) {
            *errnoptr = EFAULT;
            return -1;  // Fail, bad fsptr or fssize given
      }    

      // TODO: inode = path_resolve(fs_handle, path);
      inode = fs_handle->inode_seg;
      if (!inode) {
            *errnoptr = ENOENT;
            return -1;  // Fail, bad path given
      }    
    
      //Reset the memory of the results container
      memset(stbuf, 0, sizeof(struct stat));   

      //Populate stdbuf based on the inode
      stbuf->st_uid = uid;
      stbuf->st_gid = gid;
      stbuf->st_atim = *inode->last_acc; 
      stbuf->st_mtim = *inode->last_mod;    
      
      if (inode->is_dir) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = *inode->subdirs + 2;  // + 2 for . and ..
      } else {
            stbuf->st_mode = S_IFREG | 0755;
            stbuf->st_nlink = 1;
            stbuf->st_size = *inode->file_size_b;
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
/* Begin DEBUG  ----------------------------------------------------------- */


// Print filesystem data structure sizes
void print_struct_debug() {
    printf("File system uses the following data structures:\n");
    printf("    FSHandle        : %lu bytes\n", ST_SZ_FSHANDLE);
    printf("    Inode           : %lu bytes\n", ST_SZ_INODE);
    printf("    MemHead         : %lu bytes\n", ST_SZ_MEMHEAD);
    printf("    Data Field      : %lu bytes\n", DATAFIELD_SZ_B);
    printf("    Memory Block    : %lu bytes (%lu kb)\n", 
           MEMBLOCK_SZ_B,
           bytes_to_kb(MEMBLOCK_SZ_B));
}

typedef long unsigned int lui;

// Print memory block stats
void print_memblock_debug(MemHead *memhead) {
    printf("Memory block at %lu:\n", (lui)memhead);
    printf("    data_size_b     : %lu\n", (lui)memhead->data_size_b);
    printf("    offset_nextblk  : %lu\n", (lui)memhead->offset_nextblk);
    printf("    data            : TODO%s\n",  (char*)(memhead + DATAFIELD_SZ_B));
}

// Print inode stats
void print_inode_debug(Inode *inode) {
    char buff[100];

    printf("Inode at %lu:\n", (lui)inode);
    printf("    fname               : %s\n", inode->fname);
    printf("    is_dir              : %lu\n", (lui)inode->is_dir);
    printf("    subdirs             : %lu\n", (lui)inode->subdirs);
    printf("    file_size_b         : %lu\n", (lui)inode->file_size_b);
    // strftime(buff, sizeof buff, "%T", gmtime((void*)inode->last_acc->tv_sec));
    // printf("    last_acc            : %s.%09ld\n", buff, inode->last_acc->tv_sec);
    // strftime(buff, sizeof buff, "%T", gmtime((void*)inode->last_mod->tv_sec));
    printf("    last_mod            : %s.%09ld\n", buff, inode->last_mod->tv_sec);
    printf("    offset_firstblk     : %lu\n", (lui)inode->offset_firstblk);     
    }

// Print filesystem sta ts
void print_fs_debug(FSHandle *fs) {
    printf("File system properties: \n");
    printf("    fs (fsptr)      : %lu\n", (lui)fs);
    printf("    fs->size_b      : %lu (%lu kb)\n", fs->size_b, bytes_to_kb(fs->size_b));
    printf("    fs->magic       : %lu\n", (lui)fs->magic);
    printf("    fs->inode_seg   : %lu\n", (lui)fs->inode_seg);
    printf("    fs->mem_seg     : %lu\n", (lui)fs->mem_seg);
    // printf("Free space      : %lu bytes (%lu kb)\n", space_free(&fs), bytes_to_kb(space_free(&fs));
}


int main() 
{
    // Print welcome & struct size details
    printf("------------- File System Test Space -------------\n");
    printf("--------------------------------------------------\n\n");
    print_struct_debug();
    printf("\n");
      
    // Allocate mem space file system will occupy (usually done by m fs.c)
    size_t fssize = kb_to_bytes(16) + ST_SZ_FSHANDLE;  // kb align after handle
    void *fsptr = malloc(fssize);
    
    // Associate the filesys with a handle.
    // Note: The two vars above are used as args to the call immeidately below, 
    // Each of our 13 stubs will need a call exactly like this one to "recover"
    // the global filesystem.
    printf("Setting up filesystem for the first time...\n");
    FSHandle *fs = get_filesys_handle(fsptr, fssize);

    printf("\nSetup successful - ");
    print_fs_debug(fs);

    printf("\nExamining root inode @ ");
    print_inode_debug(fs->inode_seg);

    printf("\nExamining root dir @ ");
    print_memblock_debug(fs->mem_seg);


    printf("\nExamining inode/memblock usage -");
    Inode *inode1 = fs->inode_seg;
    MemHead *mem1 = fs->mem_seg;
    
    printf("\nIs inode 0 free = %d\n", is_inode_free(fs->inode_seg));
    printf("Is memblock 0 free = %d\n", is_memblock_free(fs->mem_seg));
    printf("\nIs inode 1 free = %d\n", is_inode_free(fs->inode_seg));
    printf("Is dir 1 free = %d\n", is_memblock_free(fs->mem_seg));

	// // Create a 8KB test file
	// create_file(&fs->root, &fs->head, 8, FS_ROOTPATH, "testfile", 0);

    // printf("\nGetting filesystem handle of existing fs...\n");
    // fs = NULL;
    // fs = get_filesys_handle(fsptr, fssize);

    // printf("Got handle successfully - ");
    // print_fs_debug(fs);


    // TODO: Write fs contents to backup file

    printf("\nExiting...\n");
    free(fsptr);

    return 0; 
} 

/* End DEBUG  ------------------------------------------------------------- */
