/* MyFS: a tiny file-system based on FUSE - Filesystem in Userspace

    Note: Requires FUSE dev lib: sudo apt-get install libfuse-dev
    
    Compile:
        gcc myfs.c implementation.c `pkg-config fuse --cflags --libs` -o myfs
    Usage (w/backup file):
        ./myfs --backupfile=test.myfs ~/test-mnt/ -f
    Usage (w/no backup file):
        ./myfs test-mnt/ -f

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

#include "implementation.h"  // File system helpers


/* Begin File System Documentation ---------------------------------------- */
/*    
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
        Simple design vs. Better design.

*/
/* End File System Documentation ------------------------------------------ */
/* Begin Filesystem Helpers ----------------------------------------------- */
Inode* resolve_path(FSHandle *fs, const char *path);
void print_memblock_debug(FSHandle *fs, MemHead *memhead);

// Returns a handle to a myfs filesystem on success.
// On fail, sets errnoptr to EFAULT and returns NULL.
static FSHandle *fs_handle(void *fsptr, size_t fssize, int *errnoptr) {
    FSHandle *fs = fs_init(fsptr, fssize);
    if (!fs && errnoptr) *errnoptr = EFAULT;
    return fs;
}

// A debug function to simulate a pathresolve() call. 
// Returns some hardcoded test inode (ex: the root dir).
// On fail, sets errnoptr to ENOENT and returns NULL.
// TODO: actual fs_pathresolve()
static Inode *fs_pathresolve(FSHandle *fs, const char *path, int *errnoptr) {
    Inode *inode = fs->inode_seg;       // /
    // Inode *inode = fs->inode_seg + 1;   // /dir1
    // Inode *inode = fs->inode_seg + 2;   // /dir1/file1
    if (!inode && errnoptr) *errnoptr = ENOENT;
    return inode;
}

/* End Filesystem Helpers ------------------------------------------------- */
/* Begin Inode helpers ---------------------------------------------------- */


// TODO: static void inode_free(FSHandle *fs, Inode *inode)

// Populates buf with a string representing the given inode's data.
// Returns: The size of the data at buf.
static size_t inode_data_get(FSHandle *fs, Inode *inode, char *buf) {
    inode_lasttimes_set(inode, 0);
    return memblock_data_get(fs, inode_firstmemblock(fs, inode), buf);   
}

// Disassociates any data from inode, formats any previously used memblocks,
// and assign the inode a new free first memblock.
static void inode_data_remove(FSHandle *fs, Inode *inode) {
    MemHead *memblock = inode_firstmemblock(fs, inode);
    MemHead *block_next;     // ptr to memblock->offset_nextblk
    void *block_end;         // End of memblock's data field

     // Format each memblock of the inode's data
    do {
        block_next = (MemHead*)ptr_from_offset(fs, memblock->offset_nextblk);
        block_end = (void*)memblock + MEMBLOCK_SZ_B;         // End of memblock
        memset(memblock, 0, (block_end - (void*)memblock));  // Format memblock
        memblock = (MemHead*)block_next;                     // Advance to next
        
    } while (block_next != (MemHead*)fs);  // i.e. memblock->offset_nextblk == 0

    // Update the inode to reflect disassociation and associate new memblock.
    inode->file_size_b = 0;
    inode->offset_firstblk = (size_t*)offset_from_ptr(fs, memblock_nextfree(fs));
    inode_lasttimes_set(inode, 1);
}

// Sets data field and updates size fields for the file or dir denoted by
// inode including handling of the  linked list of memory blocks for the data.
// Assumes: Filesystem has enough free memblocks to accomodate data.
// Assumes: inode has its offset_firstblk set.
static void inode_data_set(FSHandle *fs, Inode *inode, char *data, size_t sz) {
    // Format the inode, if needed
    if (inode->file_size_b)
        inode_data_remove(fs, inode);

    MemHead *memblock = inode_firstmemblock(fs, inode);

    // Use a single block if sz will fit in one
    if (sz <= DATAFIELD_SZ_B) {
        void *data_field = memblock + ST_SZ_MEMHEAD;
        strncpy(data_field, data, sz);
        *(int*)(&memblock->not_free) = 1;
        memblock->data_size_b = (size_t*) sz;
        memblock->offset_nextblk = 0;
        // printf("\nSET DATA:\n");
        // write(fileno(stdout), data, sz);
    }

    // Else use multiple blocks, if available
    else {
        // Determine num blocks needed
        size_t num_bytes = sz;
        
        // Debug
        // size_t blocks_needed = num_bytes / DATAFIELD_SZ_B;
        // printf("Need %lu blocks for these %lu bytes\n ", blocks_needed, num_bytes);

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

    inode_lasttimes_set(inode, 1);
    inode->file_size_b = (size_t*) sz;
}

// Appends the given data to the given Inode's current data. For appending
// a file/dir "label:offset\n" line to the directory, for example.
// No validation is performed on append_data. Assumes: append_data is a string.
// Returns 1 on success, else 0.
static int inode_data_append(FSHandle *fs, Inode *inode, char *append_data) {
    // Get the parent dir's curr data (i.e. a list of files/dirs)
    size_t data_sz = 0;
    char *data = malloc(*(int*)(&inode->file_size_b) + str_len(append_data));
    data_sz = inode_data_get(fs, inode, data);
    data_sz += str_len(append_data);  // Sz of new data field
    
    // printf("DATA for %s: '%s'\n", inode->name, data);
    // printf("APPENDING: '%s'\n", append_data);

    // Concatenate append_data to current data
    strcat(data, append_data);

    // printf("\nCALLING SET FOR: '%s' of sz: %lu\n", data, data_sz);

    inode_data_set(fs, inode, data, data_sz);  // Overwrite existing data

    // char *buf2 = malloc(inode->file_size_b);
    // inode_data_get(fs, inode, buf2);
    // printf("\nNode data after set: '%s'\n", buf2);
    // free(buf2);

    free(data);

    return 1; // Success
}


/* End Inode helpers ------------------------------------------------------ */
/* Begin Directory helpers ------------------------------------------------ */


// TODO: static void dir_data_remove(FSHandle *fs, char *path)
// TODO: static void dir_remove(FSHandle *fs, char *path)

// Returns the inode for the given item (a sub-directory or file) having the
// parent directory given by inode (Or NULL if item could not be found).
static Inode* dir_subitem_get(FSHandle *fs, Inode *inode, char *itemlabel) {
    // Get parent dir's data
    char *curr_data = malloc(*(int*)(&inode->file_size_b));
    inode_data_get(fs, inode, curr_data);

    // Get ptr to the items line in the parent dir's file/dir data.
    char *subdir_ptr = strstr(curr_data, itemlabel);

    // If subdir does not exist, return NULL
    if(subdir_ptr == NULL) {
        // printf("INFO: Sub item %s does not exist in %s.\n", itemlabel, inode->name);
        free(curr_data);
        return NULL;
    }

    // else { printf("INFO: Sub item %s exists.\n", itemlabel); }

    // Else, extract the subdir's inode offset
    char *offset_ptr = strstr(subdir_ptr, FS_DIRDATA_SEP);
    char *offsetend_ptr = strstr(subdir_ptr, FS_DIRDATA_END);

    if (!offset_ptr || !offsetend_ptr) {
        printf("ERROR: Parse fail - Dir data may be corrupt.\n");
        printf("Parent: %s Item: %s\n", inode->name, itemlabel);
        free(curr_data);
        return NULL;
    }

    size_t offset;
    size_t offset_sz = offsetend_ptr - offset_ptr;
    char *offset_str = malloc(offset_sz);
    strncpy(offset_str, offset_ptr + 1, offset_sz - 1);  // +/- 1 to exclude sep
    sscanf(offset_str, "%zu", &offset); // Convert offset from str to size_t

    // TODO: Get the inode's ptr and validate it
    Inode *subdir_inode = (Inode*)ptr_from_offset(fs, (size_t*)offset);

    // debug
    // printf("Subdir Offset: %lu\n", offset);
    // printf("Returning subdir w/name: %s\n", subdir_inode->name);

    // Cleanup
    free(curr_data);
    free(offset_str);

    return subdir_inode;
}

// Creates a new/empty sub-directory under the parent dir specified by inode.
// Returns: A ptr to the newly created dir's inode on success, else NULL.
static Inode* dir_new(FSHandle *fs, Inode *inode, char *dirname) {
    // Validate...
    if (!inode_isdir(inode)) {
        printf("ERROR: %s is not a directory\n", dirname);
        return NULL; 
    } 
    if (!inode_name_isvalid(dirname)) {
            printf("ERROR: %s is not a valid directory name\n", dirname);
            return NULL;
    }
    if(dir_subitem_get(fs, inode, dirname) != NULL) {
        printf("ERROR: %s already exists\n", dirname);
        return NULL;
    }

    // Begin creating the new directory...
    Inode *newdir_inode = inode_nextfree(fs);
    MemHead *newdir_memblock = memblock_nextfree(fs);
    newdir_inode->offset_firstblk = (size_t*)offset_from_ptr(fs, 
        (void*)newdir_memblock);

    if (newdir_inode == NULL || newdir_memblock == NULL) {
        printf("ERROR: Failed to get resources adding %s\n", dirname);
        return NULL;
    }

    // Get the new inode's offset
    char offset_str[1000];      // TODO: sz should be based on fs->num_inodes
    size_t offset = offset_from_ptr(fs, newdir_inode);        // offset
    snprintf(offset_str, sizeof(offset_str), "%lu", offset);  // offset to str

    // Build new directory's lookup line: "dirname:offset\n"
    size_t data_sz = 0;
    data_sz = str_len(dirname) + str_len(offset_str) + 2; // +2 for : and \n
    char *data = malloc(data_sz);

    strcpy(data, dirname);
    strcat(data, FS_DIRDATA_SEP);
    strcat(data, offset_str);
    strcat(data, FS_DIRDATA_END);

    //debug
    // printf("\n- Adding new dir: %s To: %s\n", dirname, inode->name);
    // printf("  New lookup line to write: %s\n", data);

    // Append the lookup line to the parent dir's existing lookup data
    inode_data_append(fs, inode, data);
    
    // Update parent dir properties
    *(int*)(&inode->subdirs) = *(int*)(&inode->subdirs) + 1;
    
    // Set new dir's properties
    inode_name_set(newdir_inode, dirname);
    *(int*)(&newdir_inode->is_dir) = 1;
    inode_data_set(fs, newdir_inode, "", 0); 

    return newdir_inode;
}


/* End Directory helpers ------------------------------------------------- */
/* Begin File helpers ------------------------------------------------------ */


// Creates a new file in the fs having the given properties.
// Note: path is parent dir path, fname is the file name. Ex: '/' and 'file1'.
// Returns: A ptr to the newly created file's I-node (or NULL on fail).
static Inode *file_new(FSHandle *fs, char *path, char *fname, char *data,
                       size_t data_sz) {
    Inode *parent = resolve_path(fs, path);

    if (!parent) {
        printf("ERROR: %s is an invalid path\n", fname);
        return NULL;
    }
    if(dir_subitem_get(fs, parent, fname) != NULL) {
        printf("ERROR: File %s already exists\n", fname);
        return NULL;
    }

    if (memblocks_numfree(fs) <  data_sz / DATAFIELD_SZ_B) {
        printf("ERROR: Insufficient free memblocks for new file %s\n", fname);
        return NULL;
    }

    Inode *inode = inode_nextfree(fs);
    if (!inode) {
        printf("ERROR: Failed getting free inode for new file %s\n", fname);
        return NULL;
    }

    MemHead *memblock = memblock_nextfree(fs);
    if (!memblock) {
        printf("ERROR: Failed getting free memblock for new file %s\n", fname);
        return NULL;
    }

    if (!inode_name_set(inode, fname)) {
        printf("ERROR: Invalid filemame for new file %s\n", fname);
        return NULL;
    }
    
    // Associate first memblock with the inode (by it's offset)
    size_t offset_firstblk = offset_from_ptr(fs, (void*)memblock);
    inode->offset_firstblk = (size_t*)offset_firstblk;
    inode_data_set(fs, inode, data, data_sz);
    
    // Update file's parent directory to include this file

    // Get the new file's inode offset
    char offset_str[100];      // TODO: sz should be based on fs->num_inodes
    size_t offset = offset_from_ptr(fs, inode);               // offset
    snprintf(offset_str, sizeof(offset_str), "%zu", offset);  // offset to str

    // Build new file's lookup line: "filename:offset\n"
    size_t fileline_sz = 0;
    fileline_sz = str_len(fname) + str_len(offset_str) + 2; // +2 for : and \n

    char *fileline_data = malloc(fileline_sz);
    strcpy(fileline_data, fname);
    strcat(fileline_data, FS_DIRDATA_SEP);
    strcat(fileline_data, offset_str);
    strcat(fileline_data, FS_DIRDATA_END);
    
    //debug
    // printf("\n- Adding new file: %s To: %s\n", fname, parent->name);
    // printf("  New lookup line to write: %s (%lu bytes)\n", fileline_data, fileline_sz);

    // Append the lookup line to the parent dir's existing lookup data
    inode_data_append(fs, parent, fileline_data);
    free(fileline_data);

    return inode;
}

// Populates buf with the file's data and returns len(buf).
static size_t file_data_get(FSHandle *fs, char *path, char *buf) {
    Inode *inode = fs_pathresolve(fs, path, NULL);
    return inode_data_get(fs, inode, buf);
}

// Removes the data from the given file and updates the necessary fs properties.
static void file_data_remove(FSHandle *fs, char *path) {
    Inode *inode = fs_pathresolve(fs, path, NULL);
    if (inode) inode_data_remove(fs, inode);
}

static int file_data_append(FSHandle *fs, char *path, char *append_data) {
    Inode *inode = fs_pathresolve(fs, path, NULL);
    return inode_data_append(fs, inode, append_data);
}

Inode* resolve_path(FSHandle *fs, const char *path) {
    Inode* root_dir = fs->inode_seg;

    // If path is root directory 
    if (strcmp(path, root_dir->name) == 0)
        return root_dir;

    Inode* curr_dir = root_dir;
    int curr_ind = 1;
    int path_ind = 0;
    int path_len = strlen(path);
    char curr_path_part[path_len];
    while (curr_ind < path_len) {
        path_ind = 0;
        while (1) {
            curr_path_part[path_ind] = path[curr_ind];
            curr_ind = curr_ind + 1;
            path_ind = path_ind + 1;
            if (path[curr_ind] == '/' || path[curr_ind] == '\0') {
                curr_ind = curr_ind + 1;
                break;
            }
        }
        curr_path_part[path_ind] = '\0';
        curr_dir = dir_subitem_get(fs, curr_dir, curr_path_part);
    }
    return curr_dir;
}

/* End File helpers ------------------------------------------------------- */
/* Begin Our 13 implementations ------------------------------------------- */


/* -- __myfs_getattr_implem() -- */
// TODO: Ready for testing.
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
*/
int __myfs_getattr_implem(void *fsptr, size_t fssize, int *errnoptr,
                          uid_t uid, gid_t gid,
                          const char *path, struct stat *stbuf) {                          
    FSHandle *fs = NULL;       // Handle to the file system
    Inode *inode = NULL;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

    //Reset the memory of the results container
    memset(stbuf, 0, sizeof(struct stat));

    //Populate stdbuf with the atrributes of the inode
    stbuf->st_uid = uid;
    stbuf->st_gid = gid;
    stbuf->st_atim = *inode->last_acc; 
    stbuf->st_mtim = *inode->last_mod;    
    
    if (inode->is_dir) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = *(int*)(&inode->subdirs) + 2;  // "+ 2" for . and .. 
    } else {
        printf("--THIS LINE SHOULD BE FOLLOWED BY A LINE 2. If not ERROR.\n");
        stbuf->st_mode = S_IFREG | 0755;
        stbuf->st_nlink = 1;
        stbuf->st_size = *inode->file_size_b;
        printf("--LINE2: All OK.\n");
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
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

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
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

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
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

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
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

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
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

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
    FSHandle *fs;           // Handle to the file system

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

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
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

    /* STUB */
    
    return -1;
}

/* -- __myfs_open_implem() -- */
// TODO: Ready for testing.
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
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(fs_pathresolve(fs, path, errnoptr)))) 
        return -1;  // Fail
    else
        return 0;   // Success
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
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

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
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

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
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

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
    FSHandle *fs;       // Handle to the file system

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    /* STUB */
    
    return -1;
}

/* End Our 13 implementations  -------------------------------------------- */
/* Begin DEBUG  ----------------------------------------------------------- */

// Print memory block stats
void print_memblock_debug(FSHandle *fs, MemHead *memhead) {
    printf("\nMemory Block -\n");
    printf("    addr            : %lu\n", (lui)memhead);
    printf("    offset          : %lu\n", (lui)offset_from_ptr(fs, memhead));
    printf("    not_free        : %lu\n", (lui)memhead->not_free);
    printf("    data_size_b     : %lu\n", (lui)memhead->data_size_b);
    printf("    offset_nextblk  : %lu\n", (lui)memhead->offset_nextblk);
    printf("    data            : %s\n",  (char*)(memhead + ST_SZ_MEMHEAD));
}

// Print inode stats
void print_inode_debug(FSHandle *fs, Inode *inode) {
    if (inode == NULL) printf("    FAIL: inode is NULL.\n");

    char *buff = malloc(*(int*)(&inode->file_size_b));
    size_t sz = inode_data_get(fs, inode, buff);
    
    printf("Inode -\n");
    printf("    addr                : %lu\n", (lui)inode);
    printf("    offset              : %lu\n", (lui)offset_from_ptr(fs, inode));
    printf("    name                : %s\n", inode->name);
    printf("    is_dir              : %lu\n", (lui)inode->is_dir);
    printf("    subdirs             : %lu\n", (lui)inode->subdirs);
    printf("    file_size_b         : %lu\n", (lui)inode->file_size_b);
    printf("    last_acc            : %09ld\n", inode->last_acc->tv_sec);
    printf("    last_mod            : %09ld\n", inode->last_mod->tv_sec);
    printf("    offset_firstblk     : %lu\n", (lui)inode->offset_firstblk);  
    printf("    data size           : %lu\n", sz); 
    printf("    data: \n");
    write(fileno(stdout), buff, sz);
    printf("\n");

    free(buff);
}

int main() 
{
    printf("------------- File System Test Space -------------\n");
    printf("--------------------------------------------------\n\n");
    print_struct_debug();
      
    /////////////////////////////////////////////////////////////////////////
    // Begin file system init  

    size_t fssize = kb_to_bytes(32) + ST_SZ_FSHANDLE;  // kb align after handle
    void *fsptr = malloc(fssize);  // Allocate fs space (usually done by myfs.c)
    
    // Associate the filesys with a handle.
    printf("\nCreating filesystem...\n");
    FSHandle *fs = fs_init(fsptr, fssize);

    printf("\n");
    print_fs_debug(fs);

    /////////////////////////////////////////////////////////////////////////
    // Begin test files/dirs

    printf("\n---- Starting Test Files/Directories -----\n");
    printf("Test Contents: /, /dir1, /dir1/file1, /file2\n");

    // File2 data: a str a's & b's & terminated with a 'c'. Spans 2 memblocks
    size_t data_sz = DATAFIELD_SZ_B * 1.25;
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
    Inode *dir1 = dir_new(fs, fs_rootnode_get(fs), "dir1");
    // Inode *file2 = file_new(fs, "/dir1", "file2", lg_data, data_sz);

    // Init test dirs/files
    printf("\nExamining / ");
    Inode *file1 = file_new(fs, "/", "file1", "hello from file 1", 17);
    Inode *file3 = file_new(fs, "/dir1", "file3", "hello from file 3", 17);
    Inode *file4 = file_new(fs, "/dir1", "file4", "hello from file 4", 17);
    // Inode *file5 = file_new(fs, "/dir1", "file5", "hello from file 5", 17);

    ////////////////////////////////////////////////////////////////////////
    // Display test file/directory attributes

    // Root dir
    printf("\nExamining / ");
    print_inode_debug(fs, fs_rootnode_get(fs));

    // Dir 1
    printf("\nExamining /dir1 ");
    print_inode_debug(fs, resolve_path(fs, "/dir1"));

    // // File1
    // printf("\nExamining /dir1/file1 ");
    // print_inode_debug(fs, resolve_path(fs, "/file1"));

    // // File 2 (screen hog)
    // printf("\nExamining /file2 ");
    // print_inode_debug(fs, resolve_path(fs, "/dir1/file2"));


    /////////////////////////////////////////////////////////////////////////
    // Cleanup
    
    printf("\n\nExiting...\n");
    free(fsptr);

    return 0; 
} 

/* End DEBUG  ------------------------------------------------------------- */
