/*

  MyFS: a tiny file-system written for educational purposes

  MyFS is Copyright 2018 University of Alaska Anchorage, College of Engineering.

  Authors: 
    Dustin Fast
    Christoph Lauter
                
  Contributors:
    Joel Keller
    Brooks Woods

  Based on:
    FUSE: Filesystem in Userspace
    Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file LICENSE.

  Usage:
    After mounting, open a new terminal and navigate to the fs at ~/fuse-mnt.

    Compile with:
      gcc myfs.c -Wall implementation.c `pkg-config fuse --cflags --libs` -o myfs
    
    Mount (w/out backup to file):
      ./myfs ~/fuse-mnt/ -f
    Mount (w/backup to file):
      ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f
    Mount (inside gdb):
      gdb --args ./myfs ~/fuse-mnt/ -f OR
      gdb --args ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f
    Unmount:
      fusermount -u ~/fuse-mnt

    For more information, see README.md.
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
#include <time.h>


/* Begin Configurables  -------------------------------------------------- */


#define FS_BLOCK_SZ_KB (4)                 // Total kbs of each memory block
#define NAME_MAXLEN (256)                  // Max length of any filename
#define BLOCKS_TO_INODES (1)               // Num of mem blocks to each inode


/* End Configurables  ---------------------------------------------------- */
/* Begin File System Definitions ----------------------------------------- */


#define BYTES_IN_KB (1024)                  // Num bytes in a kb
#define FS_PATH_SEP ("/")                   // File system's path seperator
#define FS_DIRDATA_SEP (":")                // Dir data name/offset seperator
#define FS_DIRDATA_END ("\n")               // Dir data name/offset end char
#define MAGIC_NUM (UINT32_C(0xdeadd0c5))    // Num for denoting block init

// Inode -
// An Inode represents the meta-data of a file or folder.
typedef struct Inode { 
    char name[NAME_MAXLEN];             // Inode's label (file/folder name)
    int *is_dir;                        // if 1, is a dir, else a file
    int *subdirs;                       // Subdir count (unused if not is_dir)
    size_t *file_size_b;                // File's/folder's data size, in bytes
    struct timespec *last_acc;          // File/folder last access time
    struct timespec *last_mod;          // File/Folder last modified time
    size_t *offset_firstblk;            // Byte offset from fsptr to 1st
                                        // memblock, or 0 if inode is unused
} Inode;

// Memory block header -
// Each file/dir uses one or more memory blocks.
typedef struct MemHead {
    int *not_free;                      // Denotes memblock in use (1 = used)
    size_t *data_size_b;                // Size of data field occupied
    size_t *offset_nextblk;             // Bytes offset (from fsptr) to next
                                        // memblock, if any (else 0)
} MemHead;

// Top-level filesystem handle
// A file system is a list of inodes where each knows the offset of the first 
// memory block for that file/dir.
typedef struct FSHandle {
    uint32_t magic;                     // Magic number for denoting mem init
    size_t size_b;                      // Bytes from inode seg to memblocks end
    size_t num_inodes;                  // Num inodes the file system contains
    size_t num_memblocks;               // Num memory blocks the fs contains
    struct Inode *inode_seg;            // Ptr to start of inodes segment
    struct MemHead *mem_seg;            // Ptr to start of mem blocks segment
} FSHandle;

typedef long unsigned int lui;          // For shorthand convenience in casting
static Inode* resolve_path(FSHandle *fs, const char *path);  // Prototype

// Size in bytes of the filesystem's structs (above)
#define ST_SZ_INODE sizeof(Inode)
#define ST_SZ_MEMHEAD sizeof(MemHead)
#define ST_SZ_FSHANDLE sizeof(FSHandle)  

// Size of each memory block's data field
#define DATAFIELD_SZ_B (FS_BLOCK_SZ_KB * BYTES_IN_KB - sizeof(MemHead))    

// Memory block size = MemHead + data field of size DATAFIELD_SZ_B
#define MEMBLOCK_SZ_B sizeof(MemHead) + DATAFIELD_SZ_B

// Min requestable fs size = FSHandle + 1 inode + root dir block + 1 free block
#define MIN_FS_SZ_B sizeof(FSHandle) + sizeof(Inode) + (2 * MEMBLOCK_SZ_B) 

// Offset in bytes from fsptr to start of inodes segment
#define FS_START_OFFSET sizeof(FSHandle)


/* End FS Definitions ----------------------------------------------------- */
/* Begin ptr/bytes helpers ------------------------------------------------ */


// Returns a ptr to a mem address in the file system given an offset.
static void* ptr_from_offset(FSHandle *fs, size_t *offset) {
    return (void*)((long unsigned int)fs + (size_t)offset);
}

// Returns an int offset from the filesystem's start address for the given ptr.
static size_t offset_from_ptr(FSHandle *fs, void *ptr) {
    return ptr - (void*)fs;
}

// Returns the given number of kilobytes converted to bytes.
size_t kb_to_bytes(size_t size) {
    return (size * BYTES_IN_KB);
}

// Returns the given number of bytes converted to kilobytes.
size_t bytes_to_kb(size_t size) {
    return (1.0 * size / BYTES_IN_KB);
}

// Returns 1 if given bytes are alignable on the given block_sz, else 0.
int is_bytes_blockalignable(size_t bytes, size_t block_sz) {
    if (bytes % block_sz == 0)
        return 1;
    return 0;
}

// Returns 1 if given size is alignable on the given block_sz, else 0.
int is_kb_blockaligned(size_t kbs_size, size_t block_sz) {
    return is_bytes_blockalignable(kb_to_bytes(kbs_size), block_sz);
}

/* End ptr/bytes helpers -------------------------------------------------- */
/* Begin Memblock helpers ------------------------------------------------- */


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

        memblock = (MemHead*)((size_t)memblock + MEMBLOCK_SZ_B);
    }
    return NULL;
}

// Returns the number of free memblocks in the filesystem
static size_t memblocks_numfree(FSHandle *fs) {
    Inode *inode = fs->inode_seg;
    size_t num_blocks = fs->num_memblocks;
    size_t blocks_used = 0;
    size_t node_bytes = 0;

    // Iterate each inode and determine number of blocks being used
    for (int i = 0; i < fs->num_inodes; i++) {
        node_bytes = *(size_t*)(&inode->file_size_b);

        if (node_bytes % DATAFIELD_SZ_B > 0)
            blocks_used += 1;
        blocks_used += node_bytes / DATAFIELD_SZ_B; // Note: integer division

        inode++;
    }

    return num_blocks - blocks_used;
}

// Populates buf with the given memblock's data and the data of any subsequent 
// MemBlocks extending it. Returns: The size of the data at buf.
// NOTE: buf must be pre-allocated - ex: malloc(inode->file_size_b)
size_t memblock_data_get(FSHandle *fs, MemHead *memhead, const char *buf) {
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

        // Get a ptr to memblock's data field
        char *memblock_data_field = (char *)(memblock + ST_SZ_MEMHEAD);

        // Cpy memblock's data into our buffer
        void *buf_writeat = (char *)buf + old_sz;
        memcpy(buf_writeat, memblock_data_field, sz_to_write);
        
        // If on the last (or only) memblock of the sequence, stop iterating
        if (memblock->offset_nextblk == 0) 
            break;
        // Else, go to the next memblock in the sequence
        else
            memblock = (MemHead*)ptr_from_offset(fs, memblock->offset_nextblk);
    }
    return total_sz;
}


/* End Memblock helpers -------------------------------------------------- */
/* Begin inode helpers --------------------------------------------------- */


// Sets the last access time for the given node to the current time.
// If set_modified, also sets the last modified time to the current time.
static void inode_lasttimes_set(Inode *inode, int set_modified) {
    if (!inode) return;

    struct timespec tspec;
    clock_gettime(CLOCK_REALTIME, &tspec);

    inode->last_acc = &tspec;
    if (set_modified)
        inode->last_mod = &tspec;
}

// Returns 1 if the given inode is for a directory, else 0
static int inode_isdir(Inode *inode) {
    if (inode->is_dir == 0)
        return 0;
    else
        return 1;
}

// Returns 1 unless ch is one of the following illegal naming chars:
// {, }, |, ~, DEL, :, /, \, and comma char
static int inode_name_charvalid(char ch) {
    return  (!(ch < 32 || ch == 44 || ch  == 47 || ch == 58 || ch > 122));
}

// Returns 1 iff name is legal ascii chars and within max length, else 0.
static int inode_name_isvalid(char *name) {
    int len = 0;

    for (char *c = name; *c != '\0'; c++) {
        len++;
        if (len > NAME_MAXLEN) return 0;            // Check for over max length
        if (!inode_name_charvalid(*c)) return 0;    // Check for illegal chars
    }

    if (!len) return 0;  // Zero length is invalid
    return 1;            // Valid
}

// Sets the file or directory name (of length sz) for the given inode.
// Returns: 1 on success, else 0 for invalid filename)
int inode_name_set(Inode *inode, char *name) {
    if (!inode_name_isvalid(name))
        return 0;
    strcpy(inode->name, name); 
    return 1;
}

// Returns a ptr to the given inode's first memory block, or NULL if none.
static MemHead* inode_firstmemblock(FSHandle *fs, Inode *inode) {
    return (MemHead*)ptr_from_offset(fs, inode->offset_firstblk);
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

    for (int i = 0; i < fs->num_inodes; i++) {
        if (inode_isfree(inode))
            return inode;
        inode++;
    }

    return NULL;
}


/* End inode helpers ----------------------------------------------------- */
/* Begin String Helpers -------------------------------------------------- */


// Returns the length  of the given null-terminated char array
size_t str_len(char *arr) {
    int length = 0;
    for (char *c = arr; *c != '\0'; c++)
        length++;

    return length;
}

// Returns an index to the name element of the given path. Additionaly, sets
// pathlen to the size in bytes of the path. Ex: path ='/dir1/file1' returns 6
static size_t str_name_offset(const char *path, size_t *pathlen) {
    char *start, *token, *next, *name;

    start = next = strdup(path);    // Duplicate path so we can manipulate it
    *pathlen = str_len(next);       // Set pathlen for caller
    next++;                         // Skip initial seperator
    
    while ((token = strsep(&next, FS_PATH_SEP)))
        if (!next)
            name = token;

    size_t index = name - start; 
    free(start);

    return index;
}

// Returns number of digits necessary to represent the given num as a string
static size_t digits_count(size_t num) {
    int num_digits = 0;

    while(num != 0) {
        num_digits++;
        num /= 10;
    }

    return num_digits;
}


/* End String Helpers ---------------------------------------------------- */
/* Begin filesystem helpers ---------------------------------------------- */


// Returns the root directory's inode for the given file system.
static Inode* fs_rootnode_get(FSHandle *fs) {
    return fs->inode_seg;
}

// Returns a handle to a filesystem of size fssize onto fsptr.
// If the fsptr not yet intitialized as a file system, it is formatted first.
static FSHandle* fs_init(void *fsptr, size_t size) {
    // Validate file system size
    if (size < MIN_FS_SZ_B) {
        printf("ERROR: File system size too small.\n");
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
        strncpy(root_inode->name, FS_PATH_SEP, str_len(FS_PATH_SEP));
        *(int*)(&root_inode->is_dir) = 1;
        *(int*)(&root_inode->subdirs) = 0;
        fs->inode_seg->offset_firstblk = (size_t*) (memblocks_seg - fsptr);
        *(int*)(&fs->mem_seg->not_free) = 1;
        inode_lasttimes_set(root_inode, 1);
    } 

    // Otherwise, file system already intitialized, just populate the handle
    else {
        fs->size_b = fs_size;
        fs->num_inodes = n_inodes;
        fs->num_memblocks = n_blocks;
        fs->inode_seg = (Inode*) segs_start;
        fs->mem_seg = (MemHead*) memblocks_seg;
    }

    return fs;  // Return handle to the file system
}


// Returns a handle to a myfs filesystem on success.
// On fail, sets errnoptr to EFAULT and returns NULL.
static FSHandle *fs_handle(void *fsptr, size_t fssize, int *errnoptr) {
    FSHandle *fs = fs_init(fsptr, fssize);
    if (!fs && errnoptr) *errnoptr = EFAULT;
    return fs;
}

// A debug function to simulate a resolve_path() call. 
// Returns some hardcoded test inode (ex: the root dir).
// On fail, sets errnoptr to ENOENT and returns NULL.
// Assumes: path is absolute.
static Inode *fs_pathresolve(FSHandle *fs, const char *path, int *errnoptr) {
    // Ensure at least root dir symbol present
    if (strncmp(path, FS_PATH_SEP, 1) != 0) {
        *errnoptr = EINVAL;
        return NULL;                
    }

    Inode *inode = resolve_path(fs, path);
    if (!inode && errnoptr) *errnoptr = ENOENT;
    return inode;
}


/* End Filesystem Helpers ------------------------------------------------- */
/* Begin Inode helpers ---------------------------------------------------- */


// Populates buf with a string representing the given inode's data.
// Returns: The size of the data at buf.
// NOTE: buf should be pre-sized with malloc(inode->file_size_b)
static size_t inode_data_get(FSHandle *fs, Inode *inode, const char *buf) {
    inode_lasttimes_set(inode, 0);
    return memblock_data_get(fs, inode_firstmemblock(fs, inode), buf);   
}

// Disassociates any data from inode, formats any previously used memblocks,
// and, if newblock, assign the inode a new free first memblock.
static void inode_data_remove(FSHandle *fs, Inode *inode, int newblock) {
    MemHead *memblock = inode_firstmemblock(fs, inode);
    MemHead *block_next;     // ptr to memblock->offset_nextblk
    void *block_end;         // End of memblock's data field

     // Format each memblock used by inode (implicitly sets size& not_free)
    do {
        block_next = (MemHead*)ptr_from_offset(fs, memblock->offset_nextblk);
        block_end = (void*)memblock + MEMBLOCK_SZ_B;         // End of memblock
        memset(memblock, 0, (block_end - (void*)memblock));  // Format memblock
        memblock = (MemHead*)block_next;                     // Advance to next
        
    } while (block_next != (MemHead*)fs);                    // i.e. nextblk != 0

    // Update the inode to reflect the disassociation
    *(int*)(&inode->file_size_b) = 0;
    inode_lasttimes_set(inode, 1);

    // Associate w/ new memblock, if specified
    if (newblock)
        inode->offset_firstblk = 
            (size_t*)offset_from_ptr(fs, memblock_nextfree(fs));
}

// Sets data field and updates size fields for the file or dir denoted by
// inode including handling of the  linked list of memory blocks for the data.
// Assumes: Filesystem has enough free memblocks to accomodate data.
// Assumes: inode has its offset_firstblk set.
static void inode_data_set(FSHandle *fs, Inode *inode, char *data, size_t sz) {
    // If inode has existing data or no memblock associated.
    if (inode->file_size_b || inode->offset_firstblk == 0)
        inode_data_remove(fs, inode, 1);

    MemHead *memblock = inode_firstmemblock(fs, inode);

    // Use a single block if sz will fit in one
    if (sz <= DATAFIELD_SZ_B) {
        void *data_field = memblock + ST_SZ_MEMHEAD;
        memcpy(data_field, data, sz);
        *(int*)(&memblock->not_free) = 1;
        memblock->data_size_b = (size_t*) sz;
        memblock->offset_nextblk = 0;
    }

    // Else use multiple blocks, if available
    else {
        // Determine num blocks needed
        size_t num_bytes = sz;
        
        // Populate the memory blocks with the data
        char *data_idx = data;
        MemHead *prev_block = NULL;
        size_t write_bytes = 0;
        
        while (num_bytes) {
            // Determine num bytes to write this iteration
            if (num_bytes > DATAFIELD_SZ_B)
                write_bytes = DATAFIELD_SZ_B;           // More blocks yet needed
            else
                write_bytes = num_bytes;                // Last block needed

            // Write the bytes to the data field
            char *ptr_writeto = memblock_datafield(fs, memblock);
            memcpy(ptr_writeto, data_idx, write_bytes);
            *(int*)(&memblock->not_free) = 1;
            *(size_t*)(&memblock->data_size_b) = write_bytes;

            // Update next block offsets as needed
            if (prev_block) 
                prev_block->offset_nextblk = 
                    (size_t*)offset_from_ptr(fs, (void*)memblock);
            prev_block = memblock;
            memblock->offset_nextblk = 0;
        
            // Set up the next iteration
            data_idx += write_bytes;
            num_bytes = num_bytes - write_bytes; // Adjust num bytes to write
            memblock = memblock_nextfree(fs);    // Adavance to next free block
        }
    }

    // Update access/mod times and file size
    inode_lasttimes_set(inode, 1);
    inode->file_size_b = (size_t*) sz;
}

// Appends the given data to the given Inode's current data. For appending
// a file/dir "label:offset\n" line to the directory, for example.
// No validation is performed on append_data. Assumes: append_data is a string.
static void inode_data_append(FSHandle *fs, Inode *inode, char *append_data) {
    // Get parent dir's lookup table
    size_t data_sz = 0;
    size_t append_sz = str_len(append_data);
    char *data = malloc(*(int*)(&inode->file_size_b) + append_sz);
    data_sz = inode_data_get(fs, inode, data);
    size_t total_sz = data_sz + append_sz;

    // Concat append data
    memcpy(data + data_sz, append_data, append_sz);
    inode_data_set(fs, inode, data, total_sz);
    free(data);
}


/* End Inode helpers ------------------------------------------------------ */
/* Begin Directory helpers ------------------------------------------------ */


// Returns the inode for the given item (a sub-directory or file) having the
// parent directory given by inode (Or NULL if item could not be found).
static Inode* dir_subitem_get(FSHandle *fs, Inode *inode, char *name) {
    // Get parent dir's data
    char *curr_data = malloc(*(int*)(&inode->file_size_b));
    inode_data_get(fs, inode, curr_data);

    // Get ptr to the items line in the parent dir's file/dir data.
    char *subdir_ptr = strstr(curr_data, name);

    // If subdir does not exist, return NULL
    if(subdir_ptr == NULL) {
        free(curr_data);
        return NULL;
    }

    // Else, extract the subdir's inode offset
    char *offset_ptr = strstr(subdir_ptr, FS_DIRDATA_SEP);
    char *offsetend_ptr = strstr(subdir_ptr, FS_DIRDATA_END);

    if (!offset_ptr || !offsetend_ptr) {
        printf("ERROR: Parse fail - Dir data may be corrupt -\n");
        free(curr_data);
        return NULL;
    }

    // Get the subitem's offset
    size_t offset;
    size_t offset_sz = offsetend_ptr - offset_ptr;
    char *offset_str = malloc(offset_sz);
    memcpy(offset_str, offset_ptr + 1, offset_sz - 1);  // +/- 1 excludes sep
    sscanf(offset_str, "%zu", &offset);                 // str to size_t
    Inode *subdir_inode = (Inode*)ptr_from_offset(fs, (size_t*)offset);

    // Cleanup
    free(curr_data);
    free(offset_str);
    
    if (strcmp(subdir_inode->name, name) != 0)
        return NULL;        // Path not found

    return subdir_inode;    // Success
}

// Creates a new/empty sub-directory under the parent dir specified by inode.
// Returns: A ptr to the newly created dir's inode on success, else NULL.
static Inode* dir_new(FSHandle *fs, Inode *inode, char *dirname) {
    // Validate...
    if (!inode_isdir(inode)) {
        // printf("ERROR: %s is not a directory\n", dirname);
        return NULL; 
    } 
    if (!inode_name_isvalid(dirname)) {
        // printf("ERROR: %s is not a valid directory name\n", dirname);
        return NULL;
    }
    if(dir_subitem_get(fs, inode, dirname) != NULL) {
        // printf("ERROR: %s already exists\n", dirname);
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
    size_t offset = offset_from_ptr(fs, newdir_inode);        
    char offset_str[digits_count(offset) + 1];      
    snprintf(offset_str, sizeof(offset_str), "%lu", offset);

    // Build new directory's lookup line Ex: "dirname:offset\n"
    size_t data_sz = 0;
    data_sz = str_len(dirname) + str_len(offset_str) + 2; // +2 for : and \n
    char *data = malloc(data_sz);

    strcpy(data, dirname);
    strcat(data, FS_DIRDATA_SEP);
    strcat(data, offset_str);
    strcat(data, FS_DIRDATA_END);

    // Append the lookup line to the parent dir's existing lookup table
    inode_data_append(fs, inode, data);
    
    // Update parent dir properties
    *(int*)(&inode->subdirs) = *(int*)(&inode->subdirs) + 1;
    
    // Set new dir's properties
    inode_name_set(newdir_inode, dirname);
    *(int*)(&newdir_inode->is_dir) = 1;
    inode_data_set(fs, newdir_inode, "", 0); 

    return newdir_inode;
}

// Removes the directory denoted by the given inode from the file system.
// Returns 1 on success, else 0.
static int child_remove(FSHandle *fs, const char *path) {
    Inode *parent;
    Inode *child;

    // Split the given path into seperate path and filename elements
    char *par_path, *start, *token, *next;

    start = next = strdup(path);        // Duplicate path so we can manipulate
    next++;                             // Skip initial seperator
    par_path = malloc(1);               // Init abs path str
    *par_path = '\0';

    while ((token = strsep(&next, FS_PATH_SEP))) {
        if (next) {
            par_path = realloc(par_path, str_len(par_path) + str_len(token) + 1);
            strcat(par_path, FS_PATH_SEP);
            strcat(par_path, token);
        }
    }

    if (*par_path == '\0')
        strcat(par_path, FS_PATH_SEP);  // Path is root

    // Get the parent and child inode's
    parent = resolve_path(fs, par_path);
    child = resolve_path(fs, path);

    // If valid parent/child...
    if (parent && child) {
        // Denote child's offset, in str form
        size_t dir_offset = offset_from_ptr(fs, child);
        char offset_str[digits_count(dir_offset) + 1];      
        snprintf(offset_str, sizeof(offset_str), "%lu", dir_offset);

        // Remove child's lookup line (ex: "filename:offset\n")
        char *dir_name = strdup(child->name);
        size_t data_sz = 0;
        data_sz = str_len(dir_name) + str_len(offset_str) + 2; // +2 for : & nl
        char *rmline = malloc(data_sz);

        strcpy(rmline, dir_name);
        strcat(rmline, FS_DIRDATA_SEP);
        strcat(rmline, offset_str);
        strcat(rmline, FS_DIRDATA_END);

        // Get existing parent lookup table
        char* par_data = malloc(*(int*)(&parent->file_size_b));
        size_t par_data_sz = inode_data_get(fs, parent, par_data);

        // Denote the start/end of the child's lookup line
        size_t line_sz = str_len(rmline);
        char *line_start = strstr(par_data, rmline);
        char *offsetend_ptr = line_start + line_sz;

        // Build new parent data without the child's lookup line
        char *new_data = malloc(par_data_sz - line_sz);
        size_t sz1 =  line_start - par_data;
        size_t sz2 = par_data_sz - sz1 - line_sz;
        memcpy(new_data, par_data, sz1);
        memcpy(new_data + sz1, offsetend_ptr, sz2);
        
        // Update the parent to reflect removal of child
        inode_data_set(fs, parent, new_data, sz1 + sz2);
        if (child->is_dir)
            *(int*)(&parent->subdirs) = *(int*)(&parent->subdirs) - 1;

        // Format/release the child's inode
        inode_data_remove(fs, child, 0); 
        *(int*)(&child->is_dir) = 0;
        *(int*)(&child->subdirs) = 0;

        free(par_path);
        free(start);
        free(rmline);
        free(dir_name);
    
        return 1; // Success

    } else return 0; // Fail (bad path)
}


/* End Directory helpers ------------------------------------------------- */
/* Begin File helpers ---------------------------------------------------- */


// Creates a new file in the fs having the given properties.
// Note: path is parent dir path, fname is the file name. Ex: '/' and 'file1'.
// Returns: A ptr to the newly created file's I-node (or NULL on fail).
static Inode *file_new(FSHandle *fs, char *path, char *fname, char *data,
                       size_t data_sz) {
    Inode *parent = resolve_path(fs, path);

    if (!parent) {
        // printf("ERROR: invalid path\n");
        return NULL;
    }
    
    if(dir_subitem_get(fs, parent, fname) != NULL) {
        printf("ERROR: File already exists\n");
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
        printf("ERROR: Invalid file name\n");
        return NULL;
    }
    
    // Associate first memblock with the inode (by it's offset)
    size_t offset_firstblk = offset_from_ptr(fs, (void*)memblock);
    inode->offset_firstblk = (size_t*)offset_firstblk;
    inode_data_set(fs, inode, data, data_sz);
    
    // Get the new file's inode offset and convert to str
    size_t offset = offset_from_ptr(fs, inode);
    char offset_str[digits_count(offset) + 1];      
    snprintf(offset_str, sizeof(offset_str), "%zu", offset);

    // Build new file's lookup line: "filename:offset\n"
    size_t fileline_sz = 0;
    fileline_sz = str_len(fname) + str_len(offset_str) + 2; // +2 for : and \n

    char *fileline_data = malloc(fileline_sz);
    strcpy(fileline_data, fname);
    strcat(fileline_data, FS_DIRDATA_SEP);
    strcat(fileline_data, offset_str);
    strcat(fileline_data, FS_DIRDATA_END);
    
    // Append the lookup line to the parent dir's existing lookup data
    inode_data_append(fs, parent, fileline_data);
    free(fileline_data);

    return inode;
}


// Resolves the given file or directory path and returns its associated inode.
static Inode* resolve_path(FSHandle *fs, const char *path) {
    Inode* root_dir = fs_rootnode_get(fs);

    // If path is root
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

    if (curr_dir && strcmp(curr_dir->name, curr_path_part) != 0)
        return NULL; // Path not found
    
    return curr_dir;
}

/* End File helpers ------------------------------------------------------- */
/* Begin emulation functins ----------------------------------------------- */

/* -- __myfs_getattr_implem -- */
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
                          uid_t uid, gid_t gid, const char *path, 
                          struct stat *stbuf) {         
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
    stbuf->st_atim = *(struct timespec*)(&inode->last_acc); 
    stbuf->st_mtim = *(struct timespec*)(&inode->last_mod);    
    
    if (inode->is_dir) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = *(int*)(&inode->subdirs) + 2;  // "+ 2" for . and .. 
    } else {
        stbuf->st_mode = S_IFREG | 0755;
        stbuf->st_nlink = 1;
        stbuf->st_size = *(int*)(&inode->file_size_b);
    } 

    return 0;  // Success  
}

/* -- __myfs_readdir_implem -- */
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

    // Ensure path denotes a dir
    if (!inode->is_dir) {
        *errnoptr = ENOTDIR;
        return -1;
    }

    // Get the directory's lookup table and add an extra end char to help parse
    char *data = malloc(*(int*)(&inode->file_size_b) + 1);
    size_t data_sz = inode_data_get(fs, inode, data);
    memcpy(data + data_sz + 1, FS_DIRDATA_END, 1);

    // Denote count and build content string from lookup table data
    char *token, *name, *next;
    size_t names_count = 0;
    size_t names_len = 0;

    char *names = malloc(0);
    next = name = data;
    while ((token = strsep(&next, FS_DIRDATA_END))) {
        if (!next || *token <= 64 || !inode_name_charvalid(*token)) break;

        name = token;                               // Extract file/dir name
        name = strsep(&name, FS_DIRDATA_SEP);
        int nlen = strlen(name) + 1;                // +1 for null term
        names_len += nlen;            

        // Append the file/dir name
        names = realloc(names, names_len);  
        memcpy(names + names_len - nlen, name, nlen - 1);
        memset(names + names_len - 1, '\0', 1);
        names_count++;
    }

    // Copy the items into namesptr (should combine with above, otherwise O(n^2)
    *namesptr = calloc(names_count, 1);

    if (!namesptr) {
        *errnoptr = EFAULT;
    }
    else {
        char **curr = *namesptr;  // To keep namesptr static as we iterate
        next = names;
        for (int i = 0; i < names_count; i++)
        {
            int len = str_len(next);
            *curr = realloc(*curr, len + 1);
            strcpy(*curr, next);
            next += len+1;
            *curr++;
        }
    }
    
    free(data);

    return names_count;
}

/* -- __myfs_mknod_implem -- */
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

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Ensure file does not already exist
    if (fs_pathresolve(fs, path, errnoptr)) {
        *errnoptr = EEXIST;
        return -1;
    }

    // Split the given path into seperate path and filename elements
    char *abspath, *fname, *start, *token, *next;
    
    start = next = strdup(path);    // Duplicate path so we can manipulate it
    next++;                         // Skip initial seperator
    abspath = malloc(1);            // Init abs path array
    *abspath = '\0';

    while ((token = strsep(&next, FS_PATH_SEP)))
        if (!next) {
            fname = token;
        } else {
            abspath = realloc(abspath, str_len(abspath) + str_len(token) + 1);
            strcat(abspath, FS_PATH_SEP);
            strcat(abspath, token);
        }

    // If parent is root dir
    if (*abspath == '\0')
        strcat(abspath, FS_PATH_SEP);

    // Create the file and do cleanup
    Inode *newfile = file_new(fs, abspath, fname, "", 0);
    free(abspath);
    free(start);

    if (!newfile) {
        *errnoptr = EINVAL;
        return -1;  // Fail
    }
    return 0;       // Success
}

/* -- __myfs_unlink_implem -- */
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

    if (inode->is_dir || !child_remove(fs, path)) {
        *errnoptr = EINVAL;
        return -1;  // Fail
    }

    return 0;  // Success
}

/* -- __myfs_rmdir_implem -- */
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

    // Ensure dir empty
    if (inode->file_size_b)  {
        *errnoptr = ENOTEMPTY;
        return -1;  // Fail
    }

    if (!child_remove(fs, path)) {
        *errnoptr = EINVAL;
        return -1;  // Fail
    }

    return 0;  // Success
}

/* -- __myfs_mkdir_implem -- */
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

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1;

    // Ensure file does not already exist
    if (fs_pathresolve(fs, path, errnoptr)) {
        *errnoptr = EEXIST;
        return -1;
    }

    // Seperate the dir name from the path
    char *par_path, *name, *start, *token, *next;
    
    start = next = strdup(path);    // Duplicate path so we can manipulate it
    next++;                         // Skip initial seperator
    par_path = malloc(1);           // Parent path buffer
    *par_path = '\0';

    while ((token = strsep(&next, FS_PATH_SEP)))
        if (!next) {
            name = token;
        } else {
            par_path = realloc(par_path, str_len(par_path) + str_len(token) + 1);
            strcat(par_path, FS_PATH_SEP);
            strcat(par_path, token);
        }

    // If parent is root
    if (*par_path == '\0')
        strcat(par_path, FS_PATH_SEP);

    // Create the new dir
    Inode *parent = fs_pathresolve(fs, par_path, errnoptr);
    Inode *newdir = dir_new(fs, parent, name);
    
    // Cleanup
    free(par_path);
    free(start);

    if (!newdir) {
        *errnoptr = EINVAL;
        return -1;  // Fail
    }

    return 0;       // Success
}

/* Implements an emulation of the rename system call on the filesystem 
   of size fssize pointed to by fsptr. 

   The call moves the file or directory indicated by from to to.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

    Behavior (from man 2 readme):
    If  newpath  already  exists,  it  will   be   atomically
    replaced,  so  that  there  is  no point at which another
    process attempting to access newpath will find  it  miss‐
    ing.   However,  there will probably be a window in which
    both oldpath and newpath refer to the file being renamed.

    If oldpath and newpath are existing hard links  referring
    to the same file, then rename() does nothing, and returns
    a success status.

    If newpath exists but the operation fails for  some  rea‐
    son,  rename() guarantees to leave an instance of newpath
    in place.

    oldpath can specify a directory.  In this  case,  newpath
    must either not exist, or it must specify an empty direc‐
    tory.

    If oldpath  refers  to  a  symbolic  link,  the  link  is
    renamed;  if  newpath refers to a symbolic link, the link
    will be overwritten.

   The error codes are documented in man 2 rename.

*/
int __myfs_rename_implem(void *fsptr, size_t fssize, int *errnoptr,
                         const char *from, const char *to) {
    if (strcmp(from, to) == 0) return 0;  // No work required
    
    FSHandle *fs;           // Handle to the file system

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get the indexes of the file/dir seperators and path sizes
    size_t from_len, to_len;
    size_t from_idx = str_name_offset(from, &from_len);
    size_t to_idx = str_name_offset(to, &to_len);

    char *from_path = strndup(from, (from_idx > 1 ) ? from_idx-1 : from_idx);
    char *from_name = strndup(from + from_idx, from_len - from_idx);
    char *to_path = strndup(to, to_idx);
    char *to_name = strndup(to + to_idx, to_len - to_idx);
    
    Inode *from_parent = fs_pathresolve(fs, from_path, errnoptr);
    Inode *to_parent = fs_pathresolve(fs, to_path, errnoptr);

    Inode *from_child = fs_pathresolve(fs, from, errnoptr);
    Inode *to_child = fs_pathresolve(fs, to, errnoptr);
    
    free(from_name);
    free(from_path);

    // Ensure all the boys are in the band
    if (!from_parent || !from_child || !to_parent) {
        *errnoptr = EINVAL;
        free(to_name);
        free(to_path);
        return -1;
    }

    // Begin the move... (Note: This could be done more efficiently) 
    char *data = malloc(*(int*)(&from_child->file_size_b));
    size_t sz; 

    // If renaming a directory, it must either not exist or be an empty dir
    if(from_child->is_dir) {
        // If destination doesn't exist, create it and move the data
        if(!to_child) {
            // printf("Moving !child\n");
            Inode *dest = dir_new(fs, to_parent, to_name);  // Create dest
            sz = inode_data_get(fs, from_child, data);      // Get dir's data
            inode_data_set(fs, dest, data, sz);             // Copy to dest
            child_remove(fs, from);                         // Remove old dir
        } 
        
        // If dest does exist and is empty, simply overwrite the existing data
        else if (to_child->is_dir && !to_child->file_size_b) {
            // printf("moving: to empty existing dir\n");
            sz = inode_data_get(fs, from_child, data);      // Get dir's data
            inode_data_set(fs, to_child, data, sz);         // Overwrite dest
            child_remove(fs, from);                         // Remove old dir
        }
        
        // Else, error
        else {
            *errnoptr = EINVAL;
            free(data);
            free(to_name);
            free(to_path);
            return -1;
        }
    }

    // Else, renaming a regular file
    else {
        // printf("\nmoving: file\n");

        sz = inode_data_get(fs, from_child, data);          // Get file's data
        
        // If dest exists, overwrite it in an atomic way, else just create it
        if(to_child)
            inode_data_set(fs, to_child, data, sz);         // TODO: Atomic
        else 
            file_new(fs, to_path, to_name, data, sz);
        child_remove(fs, from);                             // Remove old file
    }

    // Cleanup
    free(data);
    free(to_name);
    free(to_path);

    // printf("\nrename end\n");

    return 0;  // Success
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

    // Read file data
    char* orig_data = malloc(*(int*)(&inode->file_size_b));
    size_t data_size = inode_data_get(fs, inode, orig_data);

    // If request makes file larger
    if (offset > data_size) {
        size_t diff = offset- data_size;
        char *diff_arr = malloc(diff);
        memset(diff_arr, 0, diff);
        inode_data_append(fs, inode, diff_arr);  // Pad w/zeroes
        free(diff_arr);
    }
    // Else, if request makes file smaller
    else if (offset < data_size) {
        inode_data_set(fs, inode, orig_data, offset);
    }
    // Otherwise, file size and contents are unchanged

    return 0;  // Success
}

/* -- __myfs_open_implem -- */
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
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

    return 0; // Success
}

/* -- __myfs_read_implem -- */
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
    if (!size) return 0;    // If no bytes to read

    FSHandle *fs;           // Handle to the file system
    Inode *inode;           // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;
    
    // Read file data
    char* full_buf = malloc(*(int*)(&inode->file_size_b));
    char *cpy_buf = full_buf;
    int cpy_size = 0;
    size_t data_size = inode_data_get(fs, inode, full_buf);

    // If offset not beyond end of data
    if (offset <= data_size) {
        cpy_buf += offset;              // data index
        cpy_size = data_size - offset;  // bytes to read
        memcpy(buf, cpy_buf, cpy_size); 
    }
    else {
            *errnoptr = EFBIG;          // Max offset exceeded
    }

    free(full_buf);

    return cpy_size;  // Success
}

/* -- __myfs_write_implem -- */
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
    if (!size) return 0;  // If no bytes to write

    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

    // If offset is 0, we replace all existing data with the given data
    if (!offset) {
        char *dup = strndup(buf, size);
        inode_data_set(fs, inode, dup, size);
        free(dup);
    }
    

    // Else, append to existing file data, starting at offset
    else {   
        // Read file's existing data
        int new_data_sz;
        char *new_data;
        char *orig_data = malloc(*(int*)(&inode->file_size_b));
        size_t orig_sz = inode_data_get(fs, inode, orig_data);

        // If offset is not beyond end of data
        if (offset <= orig_sz) { 
            // Build 1st half of new_data from existing file data, starting at offset
            new_data = strndup(orig_data, offset);

            // Set 2nd half of new_data: size bytes from the buf param
            new_data_sz = offset + size;
            new_data = realloc(new_data, new_data_sz);
            memcpy(new_data + offset, buf, size);

            // Replace the file's data with the new data
            inode_data_set(fs, inode, new_data, new_data_sz);
            free(new_data);
        } 

        // Else, max offset exceeded
        else {
            *errnoptr = EFBIG;
            size = 0; // Set return value
        }

        free(orig_data);
    }

    return size;  // num bytes written
}

/* -- __myfs_utimens_implem -- */
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

    // Copy time structs to callers structs
    memcpy(inode->last_acc, &ts[0], sizeof(struct timespec));
    memcpy(inode->last_mod, &ts[1], sizeof(struct timespec));
    
    return 0;
}

/* -- __myfs_statfs_implem -- */
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
                         struct statvfs *stbuf) {
    FSHandle *fs;       // Handle to the file system

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    size_t blocks_free = memblocks_numfree(fs);
    stbuf->f_bsize = DATAFIELD_SZ_B;
    stbuf->f_blocks = 0;
    stbuf->f_bfree = blocks_free;
    stbuf->f_bavail = blocks_free;
    stbuf->f_namemax = NAME_MAXLEN;

    return 0;
}

/* End emulation functions  ----------------------------------------------- */
/* Begin DEBUG  ----------------------------------------------------------- */


// # include "debuglib.h"  // For dev use - includes main() and debug helpers


/* End DEBUG  ------------------------------------------------------------- */
