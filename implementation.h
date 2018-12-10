/*  myfs_includes.h - Definitions and helpers for the myfs file system.
                      For use by implementation.c

    Author: Dustin Fast, 2018
*/

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


    // MemHead *memblock = fs->mem_seg;
    // // size_t blks_start = (lui)fs->mem_seg;
    // // size_t blks_end = blks_start + (fs->num_memblocks * ((lui)MEMBLOCK_SZ_B));
    // size_t num_memblocks = fs->num_memblocks;
    // size_t num_free = 0;

    // for (int i = 0; i < num_memblocks; i++)
    // {
    //     if (memblock_isfree(memblock))
    //         num_free++;

    //     memblock = (MemHead*)((size_t)memblock + MEMBLOCK_SZ_B);
    // }

    // return num_free;
}

// Populates buf with the given memblock's data and the data of any subsequent 
// MemBlocks extending it. Returns: The size of the data at buf.
// NOTE: buf must be pre-allocated - ex: malloc(inode->file_size_b)
// Author: Brooks Woods & Dustin Fast
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
    return  (!(ch < 32 || ch == 44 || 
             ch == 95 || ch  == 47 || ch == 58 || ch > 122));
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
        printf(" INFO: Formatting new filesystem of size %lu bytes.\n", size);
        // printf(" (Start=%lu, end=%lu\n", (lui)fs, (lui)fs + size); // debug
        
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


/* End filesystem helpers ------------------------------------------------ */
