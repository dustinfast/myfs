/*  myfs_includes.h - A collection of definitions and helper funcs for myfs.

    Contributors: Dustin Fast
*/

#include <time.h>


/* Begin FS Definitions ---------------------------------------------------- */

#define BYTES_IN_KB (1024)                  // Num bytes in a kb
#define FS_PATH_SEP ("/")                   // File system's path seperator
#define FS_DIRDATA_SEP (":")                // Dir data name/offset seperator
#define FS_DIRDATA_END ("\n")               // Dir data name/offset end char
#define FS_BLOCK_SZ_KB (.25)                // Total kbs of each memory block
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


/* End Definitions ------------------------------------------------------- */
/* Begin String Helpers -------------------------------------------------- */

// Returns a size_t denoting the given null-terminated string's length.
size_t str_len(char *arr) {
    int length = 0;
    for (char *c = arr; *c != '\0'; c++)
        length++;

    return length;
}

/* Writes the string given by arr to stdout. */
void str_write(char *arr) {
    size_t total_written = 0;
    size_t char_count = str_len(arr);

    // Write string to stdout
    while (total_written < char_count)
        total_written += write(fileno(stdout), arr + total_written, char_count - total_written);
}

/* End String helpers ----------------------------------------------------- */
/* Begin ptr/bytes helpers ------------------------------------------------ */

// Returns a ptr to a mem address in the file system given an offset.
static void* ptr_from_offset(FSHandle *fs, size_t *offset) {
    return (void*)((long unsigned int)fs + (size_t)offset);
}

// Returns an int offset from the filesystem's start address for the given ptr.
static size_t offset_from_ptr(FSHandle *fs, void *ptr) {
    return ptr - (void*)fs;
}

/* Returns the given number of kilobytes converted to bytes. */
size_t kb_to_bytes(size_t size) {
    return (size * BYTES_IN_KB);
}

/* Returns the given number of bytes converted to kilobytes.  */
size_t bytes_to_kb(size_t size) {
    return (1.0 * size / BYTES_IN_KB);
}

/* Returns 1 if given bytes are alignable on the given block_sz, else 0. */
int is_bytes_blockalignable(size_t bytes, size_t block_sz) {
    if (bytes % block_sz == 0)
        return 1;
    return 0;
}

/* Returns 1 if given size is alignable on the given block_sz, else 0. */
int is_kb_blockaligned(size_t kbs_size, size_t block_sz) {
    return is_bytes_blockalignable(kb_to_bytes(kbs_size), block_sz);
}

/* End ptr/bytes helpers -------------------------------------------------- */





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
