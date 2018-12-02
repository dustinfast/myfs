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


    Directory file data field structure:
        Ex: "dir1:offset\ndir2:offset\nfile1:offset"
        Ex: "file1:offset\nfile2:offset"

    Design Decisions:
        Assume a single process accessing the fs at a time?
        To begin writing data before checking fs has enough room?
        Assume only absolute paths passed to 13 funcs?
        Filename and path chars to allow.


/* End File System Documentation ------------------------------------------ */
/* Begin Our Definitions -------------------------------------------------- */

#define FS_PATH_SEP ("/")                   // File system's path seperator
#define FS_DIRDATA_SEP (":")                // Dir data name/offset seperator
#define FS_DIRDATA_END ("\n")               // Dir data name/offset end char
#define FS_BLOCK_SZ_KB (.5)                 // Total kbs of each memory block
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

// Function prototypes
static int file_name_isvalid(char *fname);
static Inode* fs_get_rootinode(FSHandle *fs);
static Inode* dir_getsubitem(FSHandle *fs, Inode *inode, char *subdirname);

/* End Our Definitions ---------------------------------------------------- */
/* Begin Our Utility helpers ---------------------------------------------- */


// TODO: Move helpers to lib and just have main interface in this file

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


// TODO: static char *file_getdata(FSHandle *fs, char *path, char *buf)

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

// Populates buf with a string representing the given memblock's data,
// plus the data of any subsequent MemBlocks extending it.
// Returns: The size of the data at buf.
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

        if (!sz_to_write) 
            break;  // memblock has zero bytes of data
         
        // Resize buf to accomodate the new data
        buf = realloc(buf, total_sz);
        if (!buf) {
            printf("ERROR: Failed to realloc.");
            exit(-1);
        }

        // Get a ptr to memblock's data field
        char *memblocks_data_field = (char *)(memblock + ST_SZ_MEMHEAD);

        // Cpy memblock's data into our buffer
        void *buf_writeat = (void *)buf + old_sz;
        memcpy(buf_writeat, memblocks_data_field, sz_to_write);
        
        // Debug
        // printf("sz_to_write: %lu\n", sz_to_write);
        // printf("Memblock Data:\n");
        // write(fileno(stdout), (char *)buf_writeat, sz_to_write);
        
        // If on the last (or only) memblock of the sequence, stop iterating
        if (memblock->offset_nextblk == 0)
            break;
        
        // Else, start operating on the next memblock in the sequence
        else
            memblock = (MemHead*) ptr_from_offset(fs, memblock->offset_nextblk);
    }
    return total_sz;
}

// Sets the last access time for the given node to the current time.
// If set_modified, also sets the last modified time to the current time.
static void inode_setlasttime(Inode *inode, int set_modified) {
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
// Returns: 1 on success, else 0 (likely due to invalid filename)
int inode_set_fname(Inode *inode, char *fname) {
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
static size_t inode_getdata(FSHandle *fs, Inode *inode, char *buf) {
    inode_setlasttime(inode, 0);
    return memblock_getdata(fs, ptr_from_offset(fs, inode->offset_firstblk), buf);   
}

// Sets the data field and updates size fields for the file denoted by inode.
// Also sets up the linked list of memory blocks for the file, as needed.
// Returns: 1 on success, else 0. A 0 likely denotes a full file system.
// Assumes: Filesystem has enough free memblocks to accomodate data.
// Assumes: inode has its offset_firstblk set.
// Note: Do not call on an inode w/data assigned to it, memblocks will be lost.
static void inode_setdata(FSHandle *fs, Inode *inode, char *data, size_t sz) {
    //TODO: If inode has existing data, format and release them (see note above)

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

// Returns 1 iff fname is legal ascii chars and within max length, else 0.
static int file_name_isvalid(char *fname) {
    int len = 0;
    int ord = 0;

    for (char *c = fname; *c != '\0'; c++) {
        len++;

        // Check for over max lenght
        if (len > FNAME_MAXLEN)
            return 0;

        // Check for illegal chars ({, }, |, ~, DEL, :, /, and comma char)
        ord = (int) *c;
        if (ord < 32 || ord == 44 || ord  == 47 || ord == 58 || ord > 122)
            return 0;  
    }

    if (len)
        return 1;
}

// Given a directory or file path, returns the inode for that file or dir,
// or NULL on fail.
static Inode* file_resolvepath(FSHandle *fs, const char *path) {
    Inode *curr_inode = fs->inode_seg;  // Filesystem's first/root inode

    // If request is for root (most common case), just return the first inode
    if (path == FS_PATH_SEP) 
        return curr_inode;

    // Else, start iterating each "token" in the path to parse down to the inode
    // Ex: /dir1/file1 consists of two tokens, "dir1" and "file1"
    // Ex: /dir1/dir2 consists of two tokens, "dir1" and "dir2"
    // Ex: /dir1/dir2/ consists of two tokens, "dir1" and "dir2"
    char *curr_fname, *path_cpy, *tofree;
    tofree = path_cpy = strdup(path);  // Path working copies

    // Exclude any trailing/leading seperators
    path_cpy += str_len(path_cpy) - 1;
    if (*path_cpy == '/')
        *path_cpy = '\0';
    path_cpy = tofree;
    path_cpy++;
    
    // Determine num steps in the path, so we know when to look for last inode
    int steps = 0;
    for (char *t = path_cpy; *t != '\0'; t++)
        if (*t == *FS_PATH_SEP)
            steps++;

    // Debug
    printf("\n- Resolving path: %s\n", path_cpy);
    printf("Steps: %d\n", steps);

    while (curr_fname = strsep(&path_cpy, FS_PATH_SEP)) {
        printf("With token: '%s' at step %d\n", curr_fname, steps);  // debug

        // If steps is 0, get and return the sub inode from the curr inode
        if (steps == 0) {
            Inode *ret_node = dir_getsubitem(fs, curr_inode, curr_fname);
            if (ret_node == NULL)
                printf("Returning NULL\n");
            else
                printf("Returning: %s\n", ret_node->fname);

            return ret_node;
        }
        // Else, look up the inode for the current subdir and setup next iter
        curr_inode = dir_getsubitem(fs, curr_inode, curr_fname);
        steps--;

        if (curr_inode == NULL)
            return NULL;   // Path not found
        
    }
    free(tofree);
}

// Creates a new file in the fs having the given properties.
// Note: path is parent dir path, fname is the file name. Ex: '/' and 'file1'.
// Returns: A ptr to the newly created file's I-node (or NULL on fail).
static Inode *file_new(FSHandle *fs, char *path, char *fname, char *data, size_t data_sz) {
    // TODO: Ensure item doesn't already exist
    // if(dir_getsubitem(fs, inode, fname) != NULL) {
    //     printf("ERROR: Attempted to add a file w/name that already exists.");
    //     return NULL;
    // }

    if (memblocks_numfree(fs) <  data_sz / DATAFIELD_SZ_B)
        return NULL;  // Insufficient room in fs to accomodate data

    Inode *inode = inode_nextfree(fs); // New file's inode
    if (!inode)
        return NULL;  // Could not find a free inode

    MemHead *memblock = memblock_nextfree(fs);  // File's first memblock
    if (!memblock)
        return NULL;  // Could not find a free memblock

    if (!inode_set_fname(inode, fname))
        return NULL; // Invalid filename
    
    // Associate first memblock with the inode (by it's offset)
    size_t offset_firstblk = offset_from_ptr(fs, (void*)memblock);
    inode->offset_firstblk = (size_t*)offset_firstblk;
    inode_setdata(fs, inode, data, data_sz);
    
    // Update file's parent directory to include this file
    Inode *parentdir_inode = file_resolvepath(fs, path);
    //TODO: update parent inode dir data

    return inode;
}

// Returns the inode for the given sub-directory name having parent dir inode.
// Or NULL if sub-directory could not be found.
static Inode* dir_getsubitem(FSHandle *fs, Inode *inode, char *subdirname) {
    // Get parent dirs data
    size_t data_sz = 0;
    char *curr_data = malloc(0);
    data_sz = inode_getdata(fs, inode, curr_data);

    // Get ptr to the dir data for the requested subdir.
    char *subdir_ptr = strstr(curr_data, subdirname);

    // If subdir does not exist, return NULL
    if(subdir_ptr == NULL) {
        // printf("INFO: Subdir %s does not exist.\n", subdirname); // debug
        free(curr_data);
        return NULL;
    }

    // else { printf("INFO: Subdir %s exists.\n", subdirname); } // debug

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

// TODO: static dir_updatedata(FSHandle *fs, Inode *inode, char *new_data)

// Creates a new/empty sub-directory under the parent dir specified by inode.
// Returns: A ptr to the newly created dirs inode, else NULL.
static Inode* dir_new(FSHandle *fs, Inode *inode, char *dirname) {
    if (!inode_isdir(inode)) {
        printf("ERROR: Attempted to add a directory to a non-dir inode.");
        return NULL; 
    } else if (!file_name_isvalid(dirname)) {
            printf("ERROR: Attempted to add a directory with an invalid name.");
            return NULL;
    }  // Note: must be done even though its done by inode_set_fname later

    // If a dir of this name already exists
    if(dir_getsubitem(fs, inode, dirname) != NULL) {
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

    // Get the parent dir's list of files/dirs
    size_t data_sz = 0;
    char *data = malloc(0);
    data_sz = inode_getdata(fs, inode, data);

    // Determine sz of data field after adding the new dir
    data_sz += str_len(dirname) + str_len(offset_str) + 2; // +2 for : and \n
    
    //debug
    // printf("\n- Adding new dir: %s\n", dirname);
    // printf("  Parent data: %s\n", data);

    // Concatenate parent dir's data with new dir's data
    data = realloc(data, data_sz);
    strcat(data, dirname);
    strcat(data, FS_DIRDATA_SEP);
    strcat(data, offset_str);
    strcat(data, FS_DIRDATA_END);

    // // debug
    // printf("  Newdir inode offset: %lu\n", offset);
    // printf("  New data to write: %s\n", data);
    // printf("  New data size: %lu\n", data_sz);

    // Update parent dir properties
    inode_setdata(fs, inode, data, data_sz);  // Overwrite parent dir's data
    free(data);
    *(int*)(&inode->subdirs) = *(int*)(&inode->subdirs) + 1;

    // Debug
    // size_t data_sz2 = 0;
    // char *data2 = malloc(1);
    // data_sz2 = inode_getdata(fs, inode, data2);
    // printf("  New parent data: %s\n", data2);
    // free(data2);
    
    // Set new dir's properties
    inode_set_fname(newdir_inode, dirname);
    *(int*)(&newdir_inode->is_dir) = 1;
    inode_setdata(fs, newdir_inode, "", 0); 

    return newdir_inode;
}

// Returns the number of free bytes in the file system, according to the
// number of free memory blocks.
static size_t fs_freespace(FSHandle *fs) {
    size_t num_memblocks = memblocks_numfree(fs);
    return num_memblocks * DATAFIELD_SZ_B;
}

// Returns the root director inode for the given file system.
static Inode* fs_get_rootinode(FSHandle *fs) {
    return fs->inode_seg;
}

// Maps a filesystem of size fssize onto fsptr and returns a handle to it.
static FSHandle* fs_gethandle(void *fsptr, size_t size) {
    if (size < MIN_FS_SZ_B) return NULL; // Ensure adequate size given

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
        printf("    ** INFO: New memspace detected & formatted.\n"); // debug
        
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
        Inode *root_inode = fs_get_rootinode(fs);
        strncpy(root_inode->fname, FS_PATH_SEP, str_len(FS_PATH_SEP));
        *(int*)(&root_inode->is_dir) = 1;
        *(int*)(&root_inode->subdirs) = 0;
        fs->inode_seg->offset_firstblk = (size_t*) (memblocks_seg - fsptr);
        inode_setdata(fs, fs->inode_seg, "", 1);
        
        // debug
        // *(size_t*)(&fs->inode_seg->offset_firstblk) = (memblocks_seg - fsptr);
        // inode_setdata(fs, fs->inode_seg, "dir2:40\n", 8);
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
    fs = fs_gethandle(fsptr, fssize);
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


// Print filesystem data structure sizes
void print_struct_debug() {
    printf("File system's data structures:\n");
    printf("    FSHandle        : %lu bytes\n", ST_SZ_FSHANDLE);
    printf("    Inode           : %lu bytes\n", ST_SZ_INODE);
    printf("    MemHead         : %lu bytes\n", ST_SZ_MEMHEAD);
    printf("    Data Field      : %f bytes\n", DATAFIELD_SZ_B);
    printf("    Memory Block    : %f bytes (%lu kb)\n", 
           MEMBLOCK_SZ_B,
           bytes_to_kb(MEMBLOCK_SZ_B));
}

typedef long unsigned int lui; // Shorthand convenience

// Print memory block stats
void print_memblock_debug(FSHandle *fs, MemHead *memhead) {
    printf("Memory block at %lu:\n", (lui)memhead);
    printf("    offset          : %lu\n", (lui)offset_from_ptr(fs, memhead));
    printf("    not_free        : %lu\n", (lui)memhead->not_free);
    printf("    data_size_b     : %lu\n", (lui)memhead->data_size_b);
    printf("    offset_nextblk  : %lu\n", (lui)memhead->offset_nextblk);
    printf("    data            : %s\n",  (char*)(memhead + ST_SZ_MEMHEAD));
}

// Print inode stats
void print_inode_debug(FSHandle *fs, Inode *inode) {

    if (inode == NULL) {
        printf("    FAIL: inode is NULL.\n");
        return;
    }

    char *buf = malloc(0);
    size_t sz = inode_getdata(fs, inode, buf);
    
    printf("Inode at %lu:\n", (lui)inode);
    printf("    offset              : %lu\n", (lui)offset_from_ptr(fs, inode));
    printf("    fname               : %s\n", inode->fname);
    printf("    is_dir              : %lu\n", (lui)inode->is_dir);
    printf("    subdirs             : %lu\n", (lui)inode->subdirs);
    printf("    file_size_b         : %lu\n", (lui)inode->file_size_b);
    printf("    last_acc            : %09ld\n", inode->last_acc->tv_sec);
    printf("    last_mod            : %09ld\n", inode->last_mod->tv_sec);
    printf("    offset_firstblk     : %lu\n", (lui)inode->offset_firstblk);  
    printf("    data size           : %lu\n", sz); 
    printf("    data                : %s\n", buf); 

    // free(buf);  // TODO: This causes loss of data for subsequent calls
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
    printf("    Num Inodes      : %lu\n", inodes_numfree(fs));
    printf("    Num Memblocks   : %lu\n", memblocks_numfree(fs));
    printf("    Free space      : %lu bytes (%lu kb)\n", fs_freespace(fs), bytes_to_kb(fs_freespace(fs)));
}

int main() 
{
    /////////////////////////////////////////////////////////////////////////
    // Print welcome & struct size details
    printf("------------- File System Test Space -------------\n");
    printf("--------------------------------------------------\n\n");
    print_struct_debug();
      
    /////////////////////////////////////////////////////////////////////////
    // Begin file system init  
    printf("\n\n---- Initializing File System -----\n\n");

    size_t fssize = kb_to_bytes(16) + ST_SZ_FSHANDLE;  // kb align after handle
    void *fsptr = malloc(fssize);  // Allocate fs space (usually done by myfs.c)
    
    // Associate the filesys with a handle.
    printf("Creating filesystem...\n");
    FSHandle *fs = fs_gethandle(fsptr, fssize);

    printf("\n");
    print_fs_debug(fs);

    printf("\nExamining root inode @ ");
    print_inode_debug(fs, fs->inode_seg);

    printf("\nExamining root dir @ ");
    print_memblock_debug(fs, fs->mem_seg);


    /////////////////////////////////////////////////////////////////////////
    // Begin test files/dirs
    printf("\n\n---- Starting Test Files/Directories -----\n\n");

     // Init Dir1 - A directory in the root dir 
    Inode *dir1 = dir_new(fs, fs_get_rootinode(fs), "dir1");

    // Init File1 - a file of a single memblock
    Inode *file1 = file_new(fs, "/", "file1", "hello from file 1", 17);

    printf("\nExamining file1 (a single block file at /file1) -\n");
    print_inode_debug(fs, file1);

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
    
    // Init File2 - a file of 2 or more memblocks
    Inode *file2 = file_new(fs, "/dir1", "file2", lg_data, data_sz);

    printf("\nExamining file2 (a two-block file at /dir1/file2 -");
    print_inode_debug(fs, file2);

    printf("\nTesting resolve path, '/file1' - ");
    Inode* testnode = file_resolvepath(fs, "/file1");

    // printf("\nTesting resolve path, '/dir1' - ");
    // Inode* testinode1 = file_resolvepath(fs, "/dir1/");
    // print_inode_debug(fs, testinode1);

    // printf("\nTesting resolve path, '/dir1/' - ");
    // testinode1 = file_resolvepath(fs, "/dir1/");
    // print_inode_debug(fs, testinode1);


    // printf("\nTesting resolve path, '/dir1/file2' - ");
    // testnode = file_resolvepath(fs, "/dir1/file2");

    


    /////////////////////////////////////////////////////////////////////////
    // Cleanup
    printf("\nExiting...\n");
    free(fsptr);

    return 0; 
} 

/* End DEBUG  ------------------------------------------------------------- */
