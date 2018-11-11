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
    
    File system structure in memory is:
     ___________________________________________________________________
    |   FSHandle    |       Inodes          |       Memory Blocks       | 
    |_______________|_______________________|___________________________|
    ^               ^                       ^
    fsptr           Inode Segment           Memory Blocks segment
                    (0th is root inode)     (0th is root dir memory block)


    Memory blocks look like:
     _______________________________________
    |   MemHead    |       Data             |
    |______________|________________________|


    The data field for a folder's memory block is layed out as:
    ".:corresponding inode offset (from fsptr)\n


/* End File System Documentatin ----------------------------------------- */

/* Begin Our Definitions -------------------------------------------------- */


#define FS_ROOTPATH ("/\0")                 // File system's root path
#define FS_BLOCK_SZ_KB (1)                  // Total kbs of each memory block
#define FNAME_MAXLEN (256)                  // Max length of any filename
#define MAGIC_NUM (UINT32_C(0xdeadd0c5))    // Num for denoting block init

// Inode -
// An Inode represents the meta-data of a file or folder.
typedef struct Inode { 
    char fname[FNAME_MAXLEN];   // The file/folder's label
    int *is_dir;                // if == 1, node represents a dir, else a file
    int *subdirs;               // Subdir count (unused unless is_dir == 1)
    size_t *curr_size_b;        // Current file/dir size, in bytes
    size_t *max_size_b;         // Max sz before needing more mem blocks
    struct timespec *last_acc;  // Last access time
    struct timespec *last_mod;  // Last modified time
    size_t *firstblock_offset;  // Byte offset from fsptr to file's 1st memblock
                                // (NULL if inode is free/unused)
} Inode;

// Memory block header -
// Each file/dir uses one or more memory blocks.
typedef struct MemHead {
    int *is_free;           // Denotes this block is free
    size_t *data_size_b;    // Size of data field used
    int *offset_next;       // +/- offset to next block of file's data, if any.
                            // (data field immediately follows this field)
} MemHead;

// Top-level filesystem handle
// A file system is a list of inodes where each knows the offset of the first 
// memory block for that file/dir.
typedef struct FSHandle {
    uint32_t magic;                 // "Magic" number
    size_t size_b;                  // Fs sz from inode seg to end of mem blocks
    struct Inode *root_inode;       // Ptr to start of inode segment
    struct MemHead *root_dir;       // Ptr to start of mem blocks segment
} FSHandle;

// Size in bytes of the filesystem's structs (above)
#define ST_SZ_INODE sizeof(Inode)
#define ST_SZ_MEMHEAD sizeof(MemHead)
#define ST_SZ_FSHANDLE sizeof(FSHandle)  

// Size of each memory block's data field (ensuring kb aligned w/header) 
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
static void* ptr_from_offset(FSHandle *fs, size_t offset) {
    return (void*)((long unsigned int)fs + offset);
}

// Returns an offset from the filesystems start address for the given ptr.
// static int offset_from_ptr(FSHandle *fs, void *ptr) {
//     return ptr - fs;
// }

/* Returns 1 iff fname is legal ascii chars and within max length, else 0. */
static int is_valid_filename(char *fname) {
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

/* Sets the last access time for the given node to the current time.
If set_modified, also sets the last modified time to the current time. */
static void set_inode_lasttimes(Inode *inode, int set_modified) {
    if (!inode) return;  // Validate node

    struct timespec tspec;
    clock_gettime(CLOCK_REALTIME, &tspec);

    inode->last_acc = &tspec;
    if (set_modified)
        inode->last_mod = &tspec;
}

/* End Our Utility helpers ------------------------------------------------ */
/* Begin Our FS helpers --------------------------------------------------- */


// TODO: Inode* resolve_path(FSHandle *fs, const char *path) {
// TODO: void* get_file_data(FSHandle *fs, const char *path) {

// Sets the data field and updates size fields for file denoted by given inode.
static void* set_file_data(FSHandle *fs, Inode *inode, char *data, size_t sz) {
    MemHead *memblock = ptr_from_offset(fs, (size_t)inode->firstblock_offset);
    // int i = offset_from_ptr(fs, memblock);
    printf("    test1         : %lu\n", (long unsigned int)memblock);
    // printf("    test2         : %d\n", (long unsigned int)memblock);


    // If sz is larger than in a single block
}

/* Maps a filesystem of size fssize onto fsptr and returns the fs's handle. */
static FSHandle* get_filesys_handle(void *fsptr, size_t size) {
    if (size < MIN_FS_SZ_B) return NULL;   // Validate size specified

    FSHandle *fs = (FSHandle*) fsptr;           // Map filesys onto given mem
    size_t fs_size = size - FS_START_OFFSET;    // Space available to fs
    void *fs_start = fsptr + FS_START_OFFSET;   // Inode segment start addr
    void *root_dir_start = NULL;                // Mem block segment start addr
    void *first_memblock = NULL;                // First free mem block addr
    size_t rootdir_offset = -1;                 // Root dir offset from fsptr
    size_t firstfree_offset = -1;               // First free block offset

    // If the memory hasn't yet been initialized as a file system, do it now
    if (fs->magic != MAGIC_NUM) {
        // Determine num inodes & memblocks fs_size will allow (1:1 ratio)
        int n_blocks = 0;
        int n_inodes = 0;
        while (n_blocks * DATAFIELD_SZ_B + n_inodes * ST_SZ_INODE < fs_size) {
            n_blocks++;
            n_inodes++;
        }
        
        // Denote root dir and first free addresses abd ffsets
        root_dir_start = fs_start + (ST_SZ_INODE * n_inodes);  // 0th memblock
        first_memblock = root_dir_start + MEMBLOCK_SZ_B;    // 1th memblock

        rootdir_offset = root_dir_start - fsptr;         // 0th memblock
        firstfree_offset = rootdir_offset + MEMBLOCK_SZ_B;  // 1th memblock

         // debug
        printf("    ** New memspace detected - formatting as new file system...\n");
        printf("    Inodes                      : %d\n",n_inodes);
        printf("    Memory Blocks               : %d\n",n_blocks);
        printf("    Inodes segment start        : %lu\n", (long unsigned int)fs_start);
        printf("    Mem blocks segment start    : %lu\n", (long unsigned int)root_dir_start);
        printf("    Rootdir block offset        : %lu\n", (long unsigned int)rootdir_offset);

        // Format the entire memory space w/zero-fill
        memset(fs_start, 0, fs_size);
    
        // Populate fs data members
        fs->magic = MAGIC_NUM;
        fs->size_b = fs_size;
        fs->root_inode = (Inode*) fs_start;
        fs->root_dir = (MemHead*) root_dir_start;

        // // Set up root dir 
        // TODO: Write root dir table data w/. and ..
        fs->root_dir->is_free = (int*) 1;
        fs->root_dir->offset_next = NULL;         // Only single block used
        fs->root_dir->data_size_b = (size_t*) 0;  // TODO: Update based on data

        // Set up root inode
        strncpy(fs->root_inode->fname, FS_ROOTPATH, str_len(FS_ROOTPATH));
        fs->root_inode->is_dir = (int*) 1;
        fs->root_inode->subdirs = 0;
        fs->root_inode->curr_size_b = 0;  // TODO
        fs->root_inode->max_size_b = (size_t*) DATAFIELD_SZ_B;
        set_inode_lasttimes(fs->root_inode, 1);
        fs->root_inode->firstblock_offset = (size_t*) rootdir_offset;
        
        set_file_data(fs, fs->root_inode, "test", 4);
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
      fs_handle = get_filesys_handle(fsptr, fssize);
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
      stbuf->st_atim = *inode->last_acc; 
      stbuf->st_mtim = *inode->last_mod;    
      
      if (inode->is_dir) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = *inode->subdirs + 2;  // + 2 for . and ..
      } else {
            stbuf->st_mode = S_IFREG | 0755;
            stbuf->st_nlink = 1;
            stbuf->st_size = *inode->curr_size_b;
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
    printf("    is_free         : %lu\n", (lui)memhead->is_free);
    printf("    data_size_b     : %lu\n", (lui)memhead->data_size_b);
    printf("    offset_next     : %lu\n", (lui)memhead->offset_next);
}

// Print inode stats
void print_inode_debug(Inode *inode) {
    char buff[100];

    printf("Inode at %lu:\n", (lui)inode);
    printf("    fname               : %s\n", inode->fname);
    printf("    is_dir              : %lu\n", (lui)inode->is_dir);
    printf("    subdirs             : %lu\n", (lui)inode->subdirs);
    printf("    curr_size_b         : %lu\n", (lui)inode->curr_size_b);
    printf("    max_size_b          : %lu\n", (lui)inode->max_size_b);
    strftime(buff, sizeof buff, "%T", gmtime((void*)inode->last_acc->tv_sec));
    printf("    last_acc            : %s.%09ld\n", buff, inode->last_acc->tv_sec);
    strftime(buff, sizeof buff, "%T", gmtime((void*)inode->last_mod->tv_sec));
    printf("    last_mod            : %s.%09ld\n", buff, inode->last_mod->tv_sec);
    printf("    firstblock_offset   : %lu\n", (lui)inode->firstblock_offset);     
    }

// Print filesystem sta ts
void print_fs_debug(FSHandle *fs) {
    printf("File system properties: \n");
    printf("    fs (fsptr)      : %lu\n", (lui)fs);
    printf("    fs->size_b      : %lu (%lu kb)\n", fs->size_b, bytes_to_kb(fs->size_b));
    printf("    fs->magic       : %lu\n", (lui)fs->magic);
    printf("    fs->root_inode  : %lu\n", (lui)fs->root_inode);
    printf("    fs->root_dir    : %lu\n", (lui)fs->root_dir);
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

    printf("\nExamining root inode - The ");
    print_inode_debug(fs->root_inode);

    printf("\nExamining root dir - The ");
    print_memblock_debug(fs->root_dir);
    

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
