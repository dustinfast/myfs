/*  myfs_includes.h - Definitions and helpers for the myfs file system.
                      For use by implementation.c

    Author: Dustin Fast, 2018
*/

#include <time.h>


/* Begin Configurables  -------------------------------------------------- */


#define FS_BLOCK_SZ_KB (1)                 // Total kbs of each memory block
#define NAME_MAXLEN (256)                  // Max length of any filename
#define BLOCKS_TO_INODES (1)               // Num of mem blocks to each inode


/* End Configurables  ---------------------------------------------------- */
/* Begin File System Definitions ----------------------------------------- */


#define BYTES_IN_KB (1024)                  // Num bytes in a kb
#define FS_PATH_SEP ("/")                   // File system's path seperator
#define FS_DIRDATA_SEP (":")                // Dir data name/offset seperator
#define FS_DIRDATA_END ("b")               // Dir data name/offset end char
// #define FS_DIRDATA_END ("\n")               // Dir data name/offset end char
#define MAGIC_NUM (UINT32_C(0xdeadd0c5))    // Num for denoting block init

// Inode -
// An Inode represents the meta-data of a file or folder.
typedef struct Inode { 
    char name[NAME_MAXLEN];   // The file/folder's label
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

typedef long unsigned int lui; // For shorthand convenience in casting

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
size_t memblock_data_get(FSHandle *fs, MemHead *memhead, char *buf) {
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
        
        // Debug
        // printf("DATA GET SZ: %lu\n", total_sz);
        // printf("DATA buf: %lu\n", buf);
        // printf("DATA buf_writeat: %lu\n", buf_writeat);
        // printf("\nGOT DATA:\n");
        // write(fileno(stdout), buf, total_sz);
        // printf("\nGOT RETURNED AT BUF: %lu\n", buf);
        
        // If on the last (or only) memblock of the sequence, stop iterating
        if (memblock->offset_nextblk == 0) 
            break;
        // Else, start operating on the next memblock in the sequence
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

// Returns 1 iff name is legal ascii chars and within max length, else 0.
static int inode_name_isvalid(char *name) {
    int len = 0;
    int ord = 0;

    for (char *c = name; *c != '\0'; c++) {
        len++;

        // Check for over max length
        if (len > NAME_MAXLEN)
            return 0;

        // Check for illegal chars ({, }, |, ~, DEL, :, /, and comma char)
        ord = (int) *c;
        if (ord < 32 || ord == 44 || ord  == 47 || ord == 58 || ord > 122)
            return 0;  
    }

    if (len)
        return 1;  // Valid
    return 0;      // Invalid
}

// Sets the file or directory name (of length sz) for the given inode.
// Returns: 1 on success, else 0 for invalid filename)
// TODO: Add functionality to update parent dir if inode already had a name.
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
    size_t num_inodes = fs->num_inodes;

    for (int i = 0; i < num_inodes; i++)
    {
        if (inode_isfree(inode))
            return inode;

        inode++;
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

/* End inode helpers ----------------------------------------------------- */
/* Begin String Helpers -------------------------------------------------- */


// Returns a size_t denoting the given null-terminated string's length.
size_t str_len(char *arr) {
    int length = 0;
    for (char *c = arr; *c != '\0'; c++)
        length++;

    return length;
}


/* End String Helpers ---------------------------------------------------- */
/* Begin filesystem helpers ---------------------------------------------- */


// Returns number of free bytes in the fs, as based on num free mem blocks.
static size_t fs_freespace(FSHandle *fs) {
    size_t num_memblocks = memblocks_numfree(fs);
    return num_memblocks * DATAFIELD_SZ_B;
}

// Returns the root directory's inode for the given file system.
static Inode* fs_rootnode_get(FSHandle *fs) {
    return fs->inode_seg;
}

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
        printf(" INFO: Formatting new filesystem of size %lu bytes.\n", size);
        printf(" To use it, open a new terminal and navigate to it.\n");
        
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

    return fs;  // Return the handle to the file system
}


/* End filesystem helpers ------------------------------------------------ */
/* Begin Debug stmts  ---------------------------------------------------- */

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

// Print filesystem stats
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


/* End Filesys Debug stmts  ------------------------------------------------*/




/* End String helpers ----------------------------------------------------- */

// ----- Snippets adapted from CLauter's snippets.txt:
//

// /* A function to free memory in the style of free(), using fs to
//    describe the filesystem and an offset instead of a pointer.
// */
// static void free_memory(FileSystem* fs, __his_off_t off) {
//   void *ptr;
//   MemBlock *block;
  
//   if (fs == NULL) return;
//   if (off == ((__his_off_t) 0)) return;
//   ptr = ((void *) get_offset_to_ptr(fs, off)) - sizeof(MemBlock);
//   block = (MemBlock *) ptr;
//   return_memory_block(fs, block);
// }

// /* A function allocate memory in the style of malloc(), using fs
//    to describe the filesystem and returning an offset instead of a
//    pointer.
// */
// static __his_off_t allocate_memory(FileSystem* fs, size_t size) {
//   size_t s;
//   MemBlock *ptr;
  
//   if (fs == NULL) return ((__his_off_t) 0);
//   if (size == 0) return ((__his_off_t) 0);
//   s = size + sizeof(MemBlock);
//   if (s < size) return ((__his_off_t) 0);
//   ptr = get_memory_block(fs, s);
//   if (ptr == NULL) return ((__his_off_t) 0);
//   ptr->user_size = size;
//   return get_ptr_to_offset(fs, ((void *) ptr) + sizeof(MemBlock));
// }

// /* A function reallocate memory in the style of realloc(), using
//    fs to describe the filesystem and returning/using offsets
//    instead of pointers.
// */
// static __his_off_t reallocate_memory(FileSystem* fs, __his_off_t old_off, size_t new_size) {
//   __his_off_t new_off;
//   void *old_ptr, *new_ptr;
//   size_t copy_size, ns;
//   MemBlock *old_block;
//   MemBlock *trash;
  
//   if (fs == NULL) return ((__his_off_t) 0);
//   if (new_size == 0) {
//     free_memory(fs, old_off);
//     return ((__his_off_t) 0);
//   }
//   old_ptr = get_offset_to_ptr(fs, old_off);
//   old_block = (MemBlock *) (old_ptr - sizeof(MemBlock));
//   ns = new_size + sizeof(MemBlock);
//   if (ns >= new_size) {
//     if ((ns + sizeof(MemBlock)) >= ns) {
//       if ((old_block->size) >= (ns + sizeof(MemBlock))) {
//         trash = (MemBlock *) (((void *) old_block) + ns);
//         trash->size = old_block->size - ns;
//         trash->user_size = 0;
//         trash->next = ((__his_off_t) 0);
//         return_memory_block(fs, trash);
//         old_block->size = ns;
//         old_block->user_size = new_size;
//         return old_off;
//       }
//     }
//   }
//   if (new_size < old_block->user_size) {
//     old_block->user_size = new_size;
//     return old_off;
//   }
//   new_off = allocate_memory(fs, new_size);
//   if (new_off == ((__his_off_t) 0)) return ((__his_off_t) 0);
//   new_ptr = get_offset_to_ptr(fs, new_off);
//   copy_size = old_block->user_size;
//   if (new_size < copy_size) copy_size = new_size;
//   memcpy(new_ptr, old_ptr, copy_size);
//   free_memory(fs, old_off);
//   return new_off;
// }
