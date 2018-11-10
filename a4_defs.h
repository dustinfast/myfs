/*
a4_fs definitions - A collection of definitions and helper funcs for a4_fs.c

Contributors: Dustin Fast
*/

// #include <stddef.h>
// #include <sys/stat.h>
// #include <sys/statvfs.h>
// #include <stdint.h>
// #include <string.h>
#include <time.h>
// #include <stdlib.h>
// #include <sys/types.h>
// #include <unistd.h>
// #include <fcntl.h>
// #include <errno.h>
// #include <stdio.h>

/* Begin Definitions ---------------------------------------------------- */

#define FS_ROOTPATH ("/")                   // File system's root path
#define BYTES_IN_KB (1024)                  // Num bytes in a kb
#define BLOCK_SZ_BYTES (4 * BYTES_IN_KB)    // block size, in bytes
#define FNAME_MAXLEN (256)                  // Max length of any filename
#define MAGIC_NUM ((uint32_t) (UINT32_C(0xdeadd0c5)))

/* Inode struct -
An Inode is a file or directory header. Each contains file/dir attributes
and ptrs to either a File or Dir obejct instance. */
typedef struct Inode { 
    char filename[FNAME_MAXLEN];  // This file's (or dir's) label
    int is_dir;                   // 1 = node reprs a dir, else reprs a file
    size_t max_size_bytes;        // Max size (before grow needed)
    size_t curr_size_bytes;       // Currently used space (of max_size)
    struct timespec last_access;  // Last access time
    struct timespec last_mod;     // Last modified time
    void *first_block;            // Ptr to first MemBlock used by the file/dir
} Inode ;

/* Memory block struct -
Each file and/or folder uses a number of memory blocks. We keep track of free
blocks with FileSystem->free_blocks. */
typedef struct MemBlock {
    size_t size_bytes;      // Mem block size (BLOCK_SZ_BYTES aligned)
    size_t payload_addr;    // Start of payload memory field
    size_t payload_size;    // Size of data in payload field
    struct MemBlock *next;  // The next mem block used by the file/dir, if any
} MemBlock;

/* The Filesystem struct
    A file system is a list of inodes with each node pointing to the memory 
    address used by the file/dir the inode represents. */
typedef struct FileSystem {
    size_t size;            // Total filesystem space in bytes incl this header
    Inode *root;            // Ptr to file system's root directory inode
    Inode *free_blocks;     // Ptr to the "free mem blocks" list.
} FileSystem;

// Sizes of the above structs, for convenience
#define INODE_OBJ_SZ sizeof(Inode)    // Size of the Inode struct (bytes)
#define FILE_OBJ_SZ sizeof(File)      // Size of File struct (bytes)
#define FS_OBJ_SZ sizeof(FileSystem)  // Size of FileSystem struct (bytes)


/* End Definitions ------------------------------------------------------- */
/* Begin Utility helpers ------------------------------------------------- */


/* Returns a size_t denoting the given null-terminated string's length */
size_t str_len(char *arr) {
    int length = 0;
    for (char *c = arr; *c != '\0'; c++)
        length++;

    return length;
}

/* Returns the given number of kilobytes converted to bytes. */
size_t kb_to_bytes(size_t size) {
    return (size * BYTES_IN_KB);
}

/* Returns the given number of bytes converted to kilobytes  */
size_t bytes_to_kb(size_t size) {
    return (size / BYTES_IN_KB);
}

/* Given some size in kilobytes, returns 1 if block aligned, else returns 0 */
int is_bytes_blockaligned(size_t size) {
    if (size % BLOCK_SZ_BYTES == 0)
        return 1;
    return 0;
}

// Given some size in bytes, returns 1 if block aligned, else returns 0.
int is_kb_blockaligned(size_t size) {
    return is_bytes_blockaligned(kb_to_bytes(size));
}

/* Given a filename, returns 1 if len(fname) is within allowed filename length
    and contains no invalid chars. */
int is_valid_fname(char *fname) {
    size_t len = str_len(fname);
    if (len && len <= FNAME_MAXLEN)
        // TODO: Check for invalid chars
        return 1;
    return 0;
}


/* End Utility helpers --------------------------------------------------- */

