/* MyFS: a tiny file-system based on FUSE - Filesystem in Userspace

  Usage:
    gcc -Wall myfs.c implementation.c `pkg-config fuse --cflags --libs` -o myfs
    ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f

    May be mounted while running inside gdb (for debugging) with:
    gdb --args ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f

    It can then be unmounted (in another terminal) with
    fusermount -u ~/fuse-mnt

    Authors: Dustin Fast, Joel Keller, Brooks Woods - 2018

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


    Directory file data field structure:
        Ex: "dir1:offset\ndir2:offset\nfile1:offset"
        Ex: "file1:offset\nfile2:offset"

    Design Decisions:
        Assume a single process accesses the fs at a time?
        To begin writing data before checking fs has enough room?
        Assume only absolute paths passed to 13 funcs?
        Filename and path chars to allow.


/* End File System Documentation ------------------------------------------ */
/* Begin Function Prototypes ---------------------------------------------- */


/* End Function Prototypes ------------------------------------------------ */
/* Begin Inode helpers ---------------------------------------------------- */


// TODO: static char *inode_data_remove(FSHandle *fs, Inode *inode, char *buf)

// Sets the last access time for the given node to the current time.
// If set_modified, also sets the last modified time to the current time.
static void inode_setlasttime(Inode *inode, int set_modified) {
    if (!inode) return;

    struct timespec tspec;
    clock_gettime(CLOCK_REALTIME, &tspec);

    inode->last_acc = &tspec;
    if (set_modified)
        inode->last_mod = &tspec;
}

// Sets the file or directory name (of length sz) for the given inode.
// Returns: 1 on success, else 0 for invalid filename)
int inode_fname_set(Inode *inode, char *fname) {
    if (!file_name_isvalid(fname))
        return 0;

    strcpy(inode->fname, fname); 
    return 1;
}

// Returns 1 if the given inode is for a directory, else 0
static int inode_isdir(Inode *inode) {
    if (inode->is_dir == 0)
        return 0;
    else
        return 1;
}

// Populates buf with a string representing the given inode's data.
// Returns: The size of the data at buf.
static size_t inode_data_get(FSHandle *fs, Inode *inode, char *buf) {
    inode_setlasttime(inode, 0);
    return memblock_data_get(fs, ptr_from_offset(fs, inode->offset_firstblk), buf);   
}

// Sets data field and updates size fields for the file or dir denoted by
// inode including handling of the  linked list of memory blocks for the data.
// Assumes: Filesystem has enough free memblocks to accomodate data.
// Assumes: inode has its offset_firstblk set.
// Note: Do not call on an inode w/data assigned to it, memblocks will be lost.
static void inode_data_set(FSHandle *fs, Inode *inode, char *data, size_t sz) {
    //TODO: If inode has existing data, format and release it(see note above)

    MemHead *memblock = ptr_from_offset(fs, inode->offset_firstblk);

    // Use a single block if sz will fit in one
    if (sz <= DATAFIELD_SZ_B) {
        void *data_field = memblock + ST_SZ_MEMHEAD;
        strncpy(data_field, data, sz);
        *(int*)(&memblock->not_free) = 1;
        memblock->data_size_b = (size_t*) sz;
        memblock->offset_nextblk = 0;
    }

    // Else use multiple blocks, if available
    else {
        // Determine num blocks needed
        size_t num_bytes = sz;
        size_t blocks_needed = num_bytes / DATAFIELD_SZ_B;
        
        // printf("Need %lu blocks for these %lu bytes\n ", blocks_needed, num_bytes);  // Debug

        // Populate the memory blocks with the data
        char *data_idx = data;
        MemHead *prev_block = NULL;
        size_t write_bytes = 0;
        
        while (num_bytes) {
            // Determine num bytes to write this iteration
            if (num_bytes > DATAFIELD_SZ_B)
                write_bytes = DATAFIELD_SZ_B;  // More blocks will be needed
            else
                write_bytes = num_bytes;       // Last block needed

            // Denote ptr to write addr
            char *ptr_writeto = memblock_datafield(fs, memblock);

            // Write the bytes to the data field
            strncpy(ptr_writeto, data_idx, write_bytes);
            *(int*)(&memblock->not_free) = 1;
            *(size_t*)(&memblock->data_size_b) = write_bytes;

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

    inode_setlasttime(inode, 1);
    inode->file_size_b = (size_t*) sz;
}

// Appends the given data to the given Inode's current data. For appending
// a file/dir "label:offset\n" line to the directory, for example.
// No validation is performed on append_data. Assumes: append_data is a string.
static int inode_data_append(FSHandle *fs, Inode *inode, char *append_data) {
    // Get the parent dir's curr data (i.e. a list of files/dirs)
    size_t data_sz = 0;
    char *data = malloc(0);
    data_sz = inode_data_get(fs, inode, data);
    data_sz += str_len(append_data);  // Sz of new data field
    
    // Concatenate append_data to current data
    data = realloc(data, data_sz);
    if (!data) {
        printf("ERROR: Failed to realloc.");
        return 0;
    }
    strcat(data, append_data);

    //debug
    // printf("\n- Appending: %s\n", dirname);
    // printf("  Current data: %s\n", data);
    // printf("  Newdata: %s\n", data);

    inode_data_set(fs, inode, data, data_sz);  // Overwrite existing data
    free(data);
}

/* End Inode helpers ------------------------------------------------------ */
/* Begin File helpers ------------------------------------------------------ */

// TODO: static char *file_data_get(FSHandle *fs, char *path, char *buf)
// TODO: static char *file_data_append(FSHandle *fs, char *path, char *buf)

// Creates a new file in the fs having the given properties.
// Note: path is parent dir path, fname is the file name. Ex: '/' and 'file1'.
// Returns: A ptr to the newly created file's I-node (or NULL on fail).
static Inode *file_new(FSHandle *fs, char *path, char *fname, char *data, size_t data_sz) {
    // TODO: Get parent inode w: Inode *parentdir_inode = file_resolvepath(fs, path);
    
    // TODO: Ensure item doesn't already exist (Ready but relies on file_resolvepath())
    // if(dir_subitem_get(fs, parentdir_inode, fname) != NULL) {
    //     printf("ERROR: Attempted to add a file w/name that already exists.");
    //     return NULL;
    // }

    if (memblocks_numfree(fs) <  data_sz / DATAFIELD_SZ_B) {
        printf("ERROR: Insufficient free memblocks for new file %s\n", fname);
        return NULL;
    }

    Inode *inode = inode_nextfree(fs);
    if (!inode) {
        printf("ERROR: Failed to get a free inode for new file %s\n", fname);
        return NULL;
    }

    MemHead *memblock = memblock_nextfree(fs);
    if (!memblock) {
        printf("ERROR: Failed to get a free memblockfor new file %s\n", fname);
        return NULL;
    }

    if (!inode_fname_set(inode, fname)) {
        printf("ERROR: Invalid filemame for new file %s\n", fname);
        return NULL;
    }
    
    // Associate first memblock with the inode (by it's offset)
    size_t offset_firstblk = offset_from_ptr(fs, (void*)memblock);
    inode->offset_firstblk = (size_t*)offset_firstblk;
    inode_data_set(fs, inode, data, data_sz);
    
    // TODO: Update file's parent directory to include this file
    // TODO: as well as Update parent inode dir data.
    // TODO: (Waiting on file_resolvepath())

    return inode;
}


/* End File helpers ------------------------------------------------------- */
/* Begin Directory helpers ------------------------------------------------ */


// Returns the inode for the given item (a sub-directory or file) having the
// parent directory given by inode (Or NULL if item could not be found).
static Inode* dir_subitem_get(FSHandle *fs, Inode *inode, char *itemlabel) {
    // Get parent dirs data
    size_t data_sz = 0;
    char *curr_data = malloc(0);
    data_sz = inode_data_get(fs, inode, curr_data);

    // Get ptr to the dir data for the requested subdir.
    char *subdir_ptr = strstr(curr_data, itemlabel);

    // If subdir does not exist, return NULL
    if(subdir_ptr == NULL) {
        // printf("FSINFO: Sub item %s does not exist.\n", itemlabel); dir_subitem_get
        free(curr_data);
        return NULL;
    }

    // else { printf("FSINFO: Sub item %s exists.\n", itemlabel); } dir_subitem_get

    // Else, extract the subdir's inode offset
    char *offset_ptr = strstr(subdir_ptr, FS_DIRDATA_SEP);
    char *offsetend_ptr = strstr(subdir_ptr, FS_DIRDATA_END);

    if (!offset_ptr || !offsetend_ptr) {
        printf("ERROR: Parse fail - Dir data may be corrupt.\n");
        free(curr_data);
        return NULL;
    }

    size_t offset;
    size_t offset_sz = offsetend_ptr - offset_ptr;
    char *offset_str = malloc(offset_sz);
    strncpy(offset_str, offset_ptr + 1, offset_sz - 1);  // +/- 1 to exclude sep
    sscanf(offset_str, "%zu", &offset); // Convert offset from str to size_t

    // Get the inode's ptr and validate it
    Inode *subdir_inode = (Inode*)ptr_from_offset(fs, (size_t*)offset);

    // debug
    // printf("Subdir Offset: %lu\n", offset);
    // printf("Returning subdir w/name: %s\n", subdir_inode->fname);

    // Cleanup
    free(curr_data);
    free(offset_str);

    return subdir_inode;
}

// Creates a new/empty sub-directory under the parent dir specified by inode.
// Returns: A ptr to the newly created dir's inode on success, else NULL.
static Inode* dir_new(FSHandle *fs, Inode *inode, char *dirname) {
    // Validate params
    if (!inode_isdir(inode)) {
        printf("ERROR: Attempted to add a directory to a non-dir inode.");
        return NULL; 
    } else if (!file_name_isvalid(dirname)) {
            printf("ERROR: Attempted to add a directory with an invalid name.");
            return NULL;
    }  // Note: Should be done here even though its done by inode_fname_set later

    // If an item of this name already exists
    if(dir_subitem_get(fs, inode, dirname) != NULL) {
        printf("ERROR: Attempted to add a directory w/name that already exists.");
        return NULL;
    }

    // Else, begin creating the new directory...
    Inode *newdir_inode = inode_nextfree(fs);
    MemHead *newdir_memblock = memblock_nextfree(fs);
    newdir_inode->offset_firstblk = (size_t*)offset_from_ptr(fs, (void*)newdir_memblock);

    if (newdir_inode == NULL || newdir_memblock == NULL) {
        printf("ERROR: Failed to get free inode or memblock while adding dir.\n");
        return NULL;
    }

    // Get the new inode's offset
    char offset_str[1000];      // TODO: sz should be based on fs->num_inodes
    size_t offset = offset_from_ptr(fs, newdir_inode);        // offset
    snprintf(offset_str, sizeof(offset_str), "%zu", offset);  // offset to str

    // Build new directory's lookup line: "dirname:offset\n"
    size_t data_sz = 0;
    char *data = malloc(0);

    data_sz = str_len(dirname) + str_len(offset_str) + 2; // +2 for : and \n
    data = realloc(data, data_sz);

    strcat(data, dirname);
    strcat(data, FS_DIRDATA_SEP);
    strcat(data, offset_str);
    strcat(data, FS_DIRDATA_END);

    //debug
    // printf("\n- Adding new dir: %s\n", dirname);
    // printf("  New lookup line to write: %s\n", data);

    // Append the lookup line to the parent dir's existing lookup data
    inode_data_append(fs, inode, data);
    
    // Update parent dir properties
    *(int*)(&inode->subdirs) = *(int*)(&inode->subdirs) + 1;
    
    // Set new dir's properties
    inode_fname_set(newdir_inode, dirname);
    *(int*)(&newdir_inode->is_dir) = 1;
    inode_data_set(fs, newdir_inode, "", 0); 

    return newdir_inode;
}

/* End Directory helpers ------------------------------------------------- */
/* Begin Filesystem Helpers ---------------------------------------------- */


// Returns a handle to a filesystem of size fssize onto fsptr.
// If the fsptr not yet intitialized as a file system, it is formatted first.
static FSHandle* fs_init(void *fsptr, size_t size) {
    // Validate file system size
    if (size < MIN_FS_SZ_B) {
        printf("ERROR: Received an invalid file system size.\n");
        return NULL;
    }

    // Map file system structure onto the given memory space
    FSHandle *fs = (FSHandle*)fsptr;

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
        printf(" FSINFO: Formatting new filesystem of size %lu bytes.\n", size);
        
        // Format mem space w/zero-fill
        memset(fsptr, 0, fs_size);

        // Populate fs data members
        fs->magic = MAGIC_NUM;
        fs->size_b = fs_size;
        fs->num_inodes = n_inodes;
        fs->num_memblocks = n_blocks;
        fs->inode_seg = (Inode*) segs_start;
        fs->mem_seg = (MemHead*) memblocks_seg;

        // Set up 0th inode as the root directory having path FS_PATH_SEP
        Inode *root_inode = fs_rootnode_get(fs);
        strncpy(root_inode->fname, FS_PATH_SEP, str_len(FS_PATH_SEP));
        *(int*)(&root_inode->is_dir) = 1;
        *(int*)(&root_inode->subdirs) = 0;
        fs->inode_seg->offset_firstblk = (size_t*) (memblocks_seg - fsptr);
        inode_data_set(fs, fs->inode_seg, "", 1);
    } 

    // Otherwise, file system already intitialized, just populate the handle
    else {
        fs->size_b = fs_size;
        fs->num_inodes = n_inodes;
        fs->num_memblocks = n_blocks;
        fs->inode_seg = (Inode*) segs_start;
        fs->mem_seg = (MemHead*) memblocks_seg;
    }

    return fs;  // Return the handle to the file system
}


// Returns a handle to a myfs filesystem on success.
// On fail, sets errnoptr to EFAULT and returns NULL.
static FSHandle *fs_handle(void *fsptr, size_t fssize, int *errnoptr) {
    FSHandle *fs = fs_init(fsptr, fssize);
    if (!fs) *errnoptr = EFAULT;
    return fs;
}

// A debug function to simulate a pathresolve() call. 
// Returns some hardcoded test inode (ex the root dir).
// On fail, sets errnoptr to ENOENT and returns NULL.
// TODO: actual fs_pathresolve()
static Inode *fs_pathresolve(FSHandle *fs, const char *path, int *errnoptr) {
    Inode *inode = fs->inode_seg;
    if (!inode) *errnoptr = ENOENT;
    return inode;
}

/* End Filesystem Helpers ------------------------------------------------- */
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
    // Get a handle to the filesystem 
    FSHandle *fs;   // Handle to the file system
    Inode *inode;   // Ptr to the inode for the given path
    fs = fs_handle(fsptr, fssize, errnoptr);    // Bind filesys to memory
    if (!fs) return -1;                         // Fail: Bad fsptr or fssize  

    // Ge the inode for the given path
    inode = fs_pathresolve(fs, path, errnoptr); // Get file or dir inode
    if (!inode) return -1;                      // Fail - bad path given

    //Reset the memory of the results container
    memset(stbuf, 0, sizeof(struct stat));   

    //Populate stdbuf with the atrrdibutes of the inode
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
int __myfs_mknod_implem(void *fsptr, size_t fssize, int *errnoptr, const char *path) {
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

// Print memory block stats
void print_memblock_debug(FSHandle *fs, MemHead *memhead) {
    printf("Memory Block -\n");
    printf("    addr            : %lu\n", (lui)memhead);
    printf("    offset          : %lu\n", (lui)offset_from_ptr(fs, memhead));
    printf("    not_free        : %lu\n", (lui)memhead->not_free);
    printf("    data_size_b     : %lu\n", (lui)memhead->data_size_b);
    printf("    offset_nextblk  : %lu\n", (lui)memhead->offset_nextblk);
    printf("    data            : %s\n",  (char*)(memhead + ST_SZ_MEMHEAD));
}

// Print inode stats
void print_inode_debug(FSHandle *fs, Inode *inode) {

    if (inode == NULL)
        printf("    FAIL: inode is NULL.\n");

    char *buf = malloc(0);
    size_t sz = inode_data_get(fs, inode, buf);
    
    printf("Inode -\n");
    printf("    addr                : %lu\n", (lui)inode);
    printf("    offset              : %lu\n", (lui)offset_from_ptr(fs, inode));
    printf("    fname               : %s\n", inode->fname);
    printf("    is_dir              : %lu\n", (lui)inode->is_dir);
    printf("    subdirs             : %lu\n", (lui)inode->subdirs);
    printf("    file_size_b         : %lu\n", (lui)inode->file_size_b);
    printf("    last_acc            : %09ld\n", inode->last_acc->tv_sec);
    printf("    last_mod            : %09ld\n", inode->last_mod->tv_sec);
    printf("    offset_firstblk     : %lu\n", (lui)inode->offset_firstblk);  
    printf("    data size           : %lu\n", sz); 
    printf("    data                : ");
    if (sz)
        printf("%s", buf);
    else
        printf("NONE"); 

    // free(buf);  // TODO: This causes unexpected behavior
}

int main() 
{
    printf("------------- File System Test Space -------------\n");
    printf("--------------------------------------------------\n\n");
    print_struct_debug();
      
    /////////////////////////////////////////////////////////////////////////
    // Begin file system init  

    size_t fssize = kb_to_bytes(16) + ST_SZ_FSHANDLE;  // kb align after handle
    void *fsptr = malloc(fssize);  // Allocate fs space (usually done by myfs.c)
    
    // Associate the filesys with a handle.
    printf("\nCreating filesystem...\n");
    FSHandle *fs = fs_init(fsptr, fssize);

    printf("\n");
    print_fs_debug(fs);

    /////////////////////////////////////////////////////////////////////////
    // Begin test files/dirs

    printf("\n---- Starting Test Files/Directories -----\n");
    printf("Filesystem Contents: /, /dir1, /dir1/file1, /file2\n");

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

     // Init Dir1 - A directory in the root dir 
    Inode *dir1 = dir_new(fs, fs_rootnode_get(fs), "dir1");

    // Init File1 - a file of a single memblock
    Inode *file1 = file_new(fs, "/dir1", "file1", "hello from file 1", 17);

    // Init File2 - a file of 2 or more memblocks
    Inode *file2 = file_new(fs, "/", "file2", lg_data, data_sz);

    ////////////////////////////////////////////////////////////////////////
    // Begin test output

    // Root dir
    printf("\nExamining / ");
    print_inode_debug(fs, fs->inode_seg);

    // Dir 1
    printf("\nExamining /dir1 ");
    print_inode_debug(fs, dir1);

    // File1
    printf("\n\nExamining /dir1/file1 ");
    print_inode_debug(fs, file1);

    // File 2
    printf("\n\nExamining /file2 ");
    print_inode_debug(fs, file2);


    /////////////////////////////////////////////////////////////////////////
    // Cleanup
    
    printf("\n\nExiting...\n");
    free(fsptr);

    return 0; 
} 

/* End DEBUG  ------------------------------------------------------------- */
