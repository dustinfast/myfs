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


/* Begin File System Documentation ---------------------------------------- /*
    
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

    # TODO: Add addtl docs to README.md

    Design Decisions:
        Assume a single process accessing the fs at a time?
        To begin writing data before checking fs has enough room?


/* End File System Documentation ------------------------------------------ */
/* Begin Our Definitions -------------------------------------------------- */

#define FS_ROOTPATH ("/")                   // File system's root path
#define FS_BLOCK_SZ_KB (1)                  // Total kbs of each memory block
#define FNAME_MAXLEN (256)                  // Max length of any filename
#define BLOCKS_TO_INODES (1)                // Num of mem blocks to each inode
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
    size_t *offset_firstblk;    // Byte offset from fsptr to file/folder's 1st
                                // memblock, or 0 if inode is free/unused
} Inode;

// Memory block header -
// Each file/dir uses one or more memory blocks.
typedef struct MemHead {
    int *not_free;          // Denotes memory block in use by a file (1 = used)
    size_t *data_size_b;    // Size of data field occupied
    size_t *offset_nextblk; // Bytes offset (from fsptr) to next block of 
                            // file's data if any, else 0
} MemHead;

// Top-level filesystem handle
// A file system is a list of inodes where each knows the offset of the first 
// memory block for that file/dir.
typedef struct FSHandle {
    uint32_t magic;             // "Magic" number, for denoting mem ini'd
    size_t size_b;              // Fs sz from inode seg to end of mem blocks
    int num_inodes;             // Num inodes the file system contains
    int num_memblocks;          // Num memory blocks the file system contains
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
static void* ptr_from_offset(FSHandle *fs, size_t *offset) {
    return (void*)((long unsigned int)fs + (size_t)offset);
}

// Returns an int offset from the filesystem's start address for the given ptr.
static size_t offset_from_ptr(FSHandle *fs, void *ptr) {
    return ptr - (void*)fs;
}


/* End Our Utility helpers ------------------------------------------------ */
/* Begin Our Filesystem helpers ------------------------------------------- */


// TODO: Inode* file_resolvepath(FSHandle *fs, const char *path) {

// Returns 1 iff fname is legal ascii chars and within max length, else 0.
static int file_name_isvalid(char *fname) {
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

// Returns a ptr to a memory block's data field.
static void* memblock_datafield(FSHandle *fs, MemHead *memblock){
    return (void*)(memblock + ST_SZ_MEMHEAD);
}
// Returns 1 if the given memory block is free, else returns 0.
static int memblock_isfree(MemHead *memhead) {
    if (memhead->not_free == 0)
        return 1;
    return 0;
}

// Returns the first free memblock in the given filesystem
static MemHead* memblock_nextfree(FSHandle *fs) {
    MemHead *memblock = fs->mem_seg;
    size_t num_memblocks = fs->num_memblocks;

    for (int i = 0; i < num_memblocks; i++)
    {
        if (memblock_isfree(memblock))
            return memblock;

        memblock = memblock + (sizeof(MEMBLOCK_SZ_B) * sizeof(void*));
    }
    return NULL;
}

// Returns the number of free memblocks in the filesystem
static size_t memblocks_numfree(FSHandle *fs) {
    MemHead *memblock = fs->mem_seg;
    size_t num_memblocks = fs->num_memblocks;
    size_t num_free = 0;

    for (int i = 0; i < num_memblocks; i++)
    {
        if (memblock_isfree(memblock))
            num_free++;

        memblock = memblock + (sizeof(MEMBLOCK_SZ_B) * sizeof(void*));
    }
    return num_free;
}

/* -- memblock_getdata() -- */
/* Given FSHandle and MemHead ptrs, populates the memory pointed to by buff
   with a string representating that memblock's data, plus the data fields of
   any subsequent MemBlocks pointed to by memblock->offset_nextblk field. 
   Note: If memhead is a ptr to the first block in a file, this function
   effectively populates buf with all the data for that file.
    Accepts:
        fs      (FSHandle)  : Ptr to a filesystem in memory
        memhead (MemHead)   : Ptr to a MemHead containing desired data
        buf     (void*)     : Ptr to a dynamically allocated array w/size = 1
    Returns: 
        size_t              : Denotes the size of the data at buf  */
size_t memblock_getdata(FSHandle *fs, MemHead *memhead, char *buf) {
    MemHead *memblock = (MemHead*) memhead;
    size_t total_sz = 0;
    size_t old_sz = 0;
    size_t sz_to_write = 0;

    // Iterate each 'next' memblock until we get to one that pts no further
    while (1) {
        // Denote new required size of buf based on current pos in data
        old_sz = total_sz;
        sz_to_write = (size_t)memblock->data_size_b;
        total_sz += sz_to_write;
         
        // Resize buf to accomodate the new data
        buf = realloc(buf, total_sz);
        if (!buf) {
            printf("ERROR: Failed to realloc.");
            return 0;
        }

        // Get a ptr to memblock's data field
        char *memblocks_data_field = (char *)(memblock + ST_SZ_MEMHEAD);

        // Cpy memblock's data into our buffer
        void *buf_writeat = (void *)buf + old_sz;
        memcpy(buf_writeat, memblocks_data_field, sz_to_write);
        
        // Debug
        // printf("buf: %lu\n", (long unsigned int)buf);
        // printf("buf_writeat: %lu\n", (long unsigned int)buf_writeat);
        // printf("sz_to_write: %lu\n", sz_to_write);
        // printf("Memblock Data:\n");
        // write(fileno(stdout), (char *)buf_writeat, sz_to_write);
        // printf("\n");
        
        // If on the last (or only) memblock of the sequence, stop iterating
        if (memblock->offset_nextblk == 0)
            break;
        
        // Else, start operating on the next memblock in the sequence
        else
            memblock = (MemHead*) ptr_from_offset(fs, memblock->offset_nextblk);
    }

    // Debug
    // printf("Bytes written: %lu\n", total_sz);
    // write(fileno(stdout), buf, total_sz);

    return total_sz;
}

// Sets the last access time for the given node to the current time.
// If set_modified, also sets the last modified time to the current time.
static void inode_set_lasttime(Inode *inode, int set_modified) {
    if (!inode) return;  // Validate node

    struct timespec tspec;
    clock_gettime(CLOCK_REALTIME, &tspec);

    inode->last_acc = &tspec;
    if (set_modified)
        inode->last_mod = &tspec;
}

// Returns 1 if the given inode is free, else returns 0.
static int inode_isfree(Inode *inode) {
    if (inode->offset_firstblk == 0)
        return 1;
    return 0;
}

// Returns the first free inode in the given filesystem
static Inode* inode_nextfree(FSHandle *fs) {
    Inode *inode = fs->inode_seg;
    size_t num_inodes = fs->num_inodes;

    for (int i = 0; i < num_inodes; i++)
    {
        if (inode_isfree(inode))
            return inode;

        inode++; // ptr arithmetic
    }
    return NULL;
}

// Returns the number of free inodes in the filesystem
static size_t inodes_numfree(FSHandle *fs) {
    Inode *inode = fs->inode_seg;
    size_t num_inodes = fs->num_inodes;
    size_t num_free = 0;

    for (int i = 0; i < num_inodes; i++) {
        if (inode_isfree(inode))
            num_free++;

        inode++; // ptr arithmetic
    }
    return num_free;
}

// Sets the file or directory name (of length sz) for the given inode.
// Assumes fname is null-terminated.
void inode_set_fname(FSHandle *fs, Inode *inode, char *fname, size_t sz) {
    // TODO: Ensure valid fname
    strncpy(inode->fname, fname, sz); 
}

// Sets the data field and updates size fields for the file denoted by inode.
// Also sets up the linked list of memory blocks for the file, as needed.
// Returns 1 on success, else 0. A 0 likely denotes a full file system.
// Assumes: Filesystem has enough free memblocks to accomodate data.
// Assumes: inode has its offset_firstblk set.
// Assumes: inode has not previously had data assigned to it.
static int inode_setdata(FSHandle *fs, Inode *inode, char *data, size_t sz) {
    MemHead *memblock = ptr_from_offset(fs, inode->offset_firstblk);

    // Use a single block if sz will fit in one
    if (sz <= DATAFIELD_SZ_B) {
        void *data_field = memblock + ST_SZ_MEMHEAD;
        strncpy(data_field, data, sz);
        memblock->not_free = (int*)1;
        memblock->data_size_b = (size_t*) sz;
        memblock->offset_nextblk = 0;
    }

    // Else use multiple blocks, if available
    else {
        // Determine num blocks needed
        size_t num_bytes = sz;
        size_t memblock_bytes = num_bytes - DATAFIELD_SZ_B;
        size_t blocks_needed = num_bytes / DATAFIELD_SZ_B;
        
        // Debug
        // printf("Need %lu blocks for these %lu bytes\n ", blocks_needed, num_bytes);

        // Populate the memory blocks with the data
        char *data_idx = data;
        MemHead *prev_block = NULL;
        size_t write_bytes = 0;
        
        while (num_bytes) {
            // Determine num bytes to write this iteration
            if (num_bytes > DATAFIELD_SZ_B)
                write_bytes = DATAFIELD_SZ_B;  // More blocks needed
            else
                write_bytes = num_bytes;       // Last block needed

            // Denote ptr to write addr
            char *ptr_writeto = memblock_datafield(fs, memblock);

            // Write the bytes to the data field
            strncpy(ptr_writeto, data_idx, write_bytes);
            memblock->not_free = (int*)1;
            memblock->data_size_b = (size_t*)write_bytes;

            // Update next block offsets as needed
            if (prev_block) 
                prev_block->offset_nextblk = (size_t*)offset_from_ptr(fs, (void*)memblock);
            prev_block = memblock;
            memblock->offset_nextblk = 0;
        
            // Set up the next iteration
            data_idx += write_bytes;
            num_bytes = num_bytes - write_bytes; // Adjust num bytes to write
            memblock = memblock_nextfree(fs); // Adavance to next free memblock
        }
    }

    inode_set_lasttime(inode, 1);
    inode->file_size_b = (size_t*) sz;

    return 1;
}

// Maps a filesystem of size fssize onto fsptr and returns a handle to it.
static FSHandle* fs_get_handle(void *fsptr, size_t size) {
    if (size < MIN_FS_SZ_B) return NULL; // Ensure adequate size given

    // Map file system structure onto the given memory space
    FSHandle *fs = (FSHandle*) fsptr;

    size_t fs_size = size - FS_START_OFFSET;    // Space available to fs
    void *segs_start = fsptr + FS_START_OFFSET; // Start of fs's segments
    void *memblocks_seg = NULL;                 // Mem block segment start addr
    int n_inodes = 0;                           // Num inodes fs contains
    int n_blocks = 0;                           // Num memblocks fs contains

    // Determine num inodes & memblocks that will fit in the given size
    while (n_blocks * DATAFIELD_SZ_B + n_inodes * ST_SZ_INODE < fs_size) {
        n_inodes++;
        n_blocks += BLOCKS_TO_INODES;
    }
    
    // Denote memblocks addr & offset
    memblocks_seg = segs_start + (ST_SZ_INODE * n_inodes);
    
    // If first bytes aren't our magic number, format the mem space for the fs
    if (fs->magic != MAGIC_NUM) {
        printf("    ** DEBUG: New memspace detected & formatted.\n"); // debug
        
        // Format mem space w/zero-fill
        memset(fsptr, 0, fs_size);                  

        // Populate fs data members
        fs->magic = MAGIC_NUM;
        fs->size_b = fs_size;
        fs->num_inodes = n_inodes;
        fs->num_memblocks = n_blocks;
        fs->inode_seg = (Inode*) segs_start;
        fs->mem_seg = (MemHead*) memblocks_seg;

        // Set up 0th inode as the root inode
        strncpy(fs->inode_seg->fname, FS_ROOTPATH, str_len(FS_ROOTPATH));
        fs->inode_seg->is_dir = (int*) 1;
        fs->inode_seg->subdirs = 0;
        fs->inode_seg->offset_firstblk = (size_t*) (memblocks_seg - fsptr);
        
        // Set up 0th memory block as the root directory
        // TODO: Write root dir table
        inode_setdata(fs, fs->inode_seg, "hello world", 11);  // debug
    } 

    // Otherwise, just update the handle info
    else {
        fs->size_b = fs_size;
        fs->num_inodes = n_inodes;
        fs->num_memblocks = n_blocks;
        fs->inode_seg = (Inode*) segs_start;
        fs->mem_seg = (MemHead*) memblocks_seg;
    }

    return fs;   
}


/* End Filesystem helpers ------------------------------------------------ */
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
    FSHandle *fs;   // Handle to the file system
    Inode *inode;   // Ptr to the inode for the given path

    // Bind fs to the filesystem
    fs = fs_get_handle(fsptr, fssize);
    if (!fs) {
        *errnoptr = EFAULT;
        return -1;  // Fail - bad fsptr or fssize given
    }    

    // TODO: inode = path_resolve(fs, path) instead of fs->inode_seg
    inode = fs->inode_seg;
    if (!inode) {
        *errnoptr = ENOENT;
        return -1;  // Fail - bad path given
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
        stbuf->st_nlink = *inode->subdirs + 2;  // "+ 2" for . and .. 
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
    printf("File system's data structures:\n");
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
void print_memblock_debug(FSHandle *fs, MemHead *memhead) {
    printf("Memory block at %lu:\n", (lui)memhead);
    printf("    offset          : %lu\n", (lui)offset_from_ptr(fs, memhead));
    printf("    data_size_b     : %lu\n", (lui)memhead->data_size_b);
    printf("    offset_nextblk  : %lu\n", (lui)memhead->offset_nextblk);
    printf("    data            : %s\n",  (char*)(memhead + ST_SZ_MEMHEAD));
}

// Print inode stats
void print_inode_debug(FSHandle *fs, Inode *inode) {
    char buff[100];

    printf("Inode at %lu:\n", (lui)inode);
    printf("    fname               : %s\n", inode->fname);
    printf("    is_dir              : %lu\n", (lui)inode->is_dir);
    printf("    subdirs             : %lu\n", (lui)inode->subdirs);
    printf("    file_size_b         : %lu\n", (lui)inode->file_size_b);
    // TODO: Test last_acc and last_mod
    // strftime(buff, sizeof buff, "%T", gmtime((void*)inode->last_acc->tv_sec));
    // printf("    last_acc            : %s.%09ld\n", buff, inode->last_acc->tv_sec);
    // strftime(buff, sizeof buff, "%T", gmtime((void*)inode->last_mod->tv_sec));
    // printf("    last_mod            : %s.%09ld\n", buff, inode->last_mod->tv_sec);
    printf("    offset_firstblk     : %lu\n", (lui)inode->offset_firstblk);  
    
    char *buf = malloc(1);
    size_t sz = memblock_getdata(fs, ptr_from_offset(fs, inode->offset_firstblk), buf);
    
    printf("    data size           : %lu\n", sz); 
    printf("    data                :\n");    
    
    write(fileno(stdout), buf, sz);
    printf("\n");
    }

// Print filesystem sta ts
void print_fs_debug(FSHandle *fs) {
    printf("File system properties: \n");
    printf("    fs (fsptr)      : %lu\n", (lui)fs);
    printf("    fs->num_inodes  : %lu\n", (lui)fs->num_inodes);
    printf("    fs->num_memblks : %lu\n", (lui)fs->num_memblocks);
    printf("    fs->size_b      : %lu (%lu kb)\n", fs->size_b, bytes_to_kb(fs->size_b));
    printf("    fs->inode_seg   : %lu\n", (lui)fs->inode_seg);
    printf("    fs->mem_seg     : %lu\n", (lui)fs->mem_seg);
    // TODO: printf("Free space      : %lu bytes (%lu kb)\n", space_free(&fs), bytes_to_kb(space_free(&fs));
}

// Creates a new file in the fs having the given properties.
// Returns: A ptr to the newly created file's I-node.
// Assumes: path and file name are null-terminated.
static Inode *file_new(FSHandle *fs, char *path, char *fname, char *data, size_t data_sz) {
    // Ensure adequete room in fs

    Inode *newfile_inode = inode_nextfree(fs);
    // TODO: Validate fname
    inode_set_fname(fs, newfile_inode, "fname", 7);
    size_t offset_firstblk = offset_from_ptr(fs, (void*)memblock_nextfree(fs));
    newfile_inode->offset_firstblk = (size_t*) offset_firstblk;
    inode_setdata(fs, newfile_inode, data, data_sz);
    return newfile_inode;
    }

// TODO: static char* file_get_data(FSHandle *fs, const char *path) {
// TODO: static dir_createnew(FSHandle *fs, const char *path, const char *dirname)
// TODO: static char* inode_get_data


int main() 
{
    // Print welcome & struct size details
    printf("------------- File System Test Space -------------\n");
    printf("--------------------------------------------------\n\n");
    print_struct_debug();
    printf("\n");
      
    // Allocate mem space file system will occupy (usually done by myfs.c)
    size_t fssize = kb_to_bytes(16) + ST_SZ_FSHANDLE;  // kb align after handle
    void *fsptr = malloc(fssize);
    
    // Associate the filesys with a handle.
    printf("Creating filesystem...\n");
    FSHandle *fs = fs_get_handle(fsptr, fssize);

    printf("\n");
    print_fs_debug(fs);

    printf("\nExamining root inode @ ");
    print_inode_debug(fs, fs->inode_seg);

    printf("\nExamining root dir @ ");
    print_memblock_debug(fs, fs->mem_seg);

    printf("\nExamining inode & memblock usage -\n");
    printf("    Is inode 0 free = %d\n", inode_isfree(fs->inode_seg));
    printf("    Is inode 1 free = %d\n", inode_isfree(fs->inode_seg + 1));
    printf("    Is memblock 0 free = %d\n", memblock_isfree(fs->mem_seg));
    printf("    Is memblock 1 free = %d\n", memblock_isfree(fs->mem_seg + 1));

    /////////////////////////////////////////////////////////////////////////
    // Begin Single memblock Test File - 
    printf("\n\n---- Starting Test File 1 (single memory block) -----\n");

    // Create File2
    Inode *file1 = file_new(fs, "/\0", "file1\0", "hello from file 1", 17);

    printf("\nExamining file1 -\n");
    print_inode_debug(fs, file1);


    /////////////////////////////////////////////////////////////////////////
    // Begin Multi-memblock Test File -
    printf("\n\n---- Starting Test File 2 (multi memory block) -----\n");

    // Build file2 data: a str of half a's, half b's, and terminated with a 'c'
    size_t data_sz = (DATAFIELD_SZ_B * 1) + 10;  // Larger than 1 memblock
    char *lg_data = malloc(data_sz);
    for (size_t i = 0; i < data_sz; i++) {
        char *c = lg_data + i;

        if (i < data_sz / 2)
            *c = 'a';
        else if (i == data_sz - 1)
            *c = 'c';
        else
            *c = 'b';
    }

    // Create File2
    Inode *file2 = file_new(fs, "/\0", "file2\0", lg_data, data_sz);

    // Display file2 properties for verification
    printf("\nExamining file2 - ");
    print_inode_debug(fs, file2);

    printf("\nExiting...\n");
    free(fsptr);

    return 0; 
} 

/* End DEBUG  ------------------------------------------------------------- */
