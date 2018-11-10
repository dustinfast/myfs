/*
a4_fs definitions - A collection of definitions and helper funcs for a4_fs.c

Contributors: Dustin Fast
*/


/* Begin Definitions ---------------------------------------------------- */

#define FS_ROOTPATH ("/")                   // File system's root path
#define BYTES_IN_KB (1024)                  // Num bytes in a kb
#define BLOCK_SZ_BYTES (4 * BYTES_IN_KB)    // block size, in bytes
#define FNAME_MAXLEN (256)                  // Max length of any filename

/* File system's I-Node struct -
    Files have a inode numbers assigned to them. 
    Folders are a list of file objects that have there associated inodes.*/
typedef struct Inode { 
    int inode_number;       // This Inode's index
    void *block_start;      // Pointer to start address of virtual mem block 
    int is_free;            // Denotes block is free
    struct Inode *next;     // Next Inode
} Inode ;

/* File system's File struct -
    Files are the filesystem's main container, used to represent either an
    individual file (each w/an inode # assigned) or a folder of files (each
    folder is simply a list of Files). */
typedef struct File {
    char filename[FNAME_MAXLEN];  // This file's (or folder's) name
    void *starting_inode;   // First inode (we should keep inodes consecutive)
    size_t filesize;        // File length in KB (must be 4KB block sz aligned)
    int is_dir;             // Denotes this File is a directory
    struct File *next;      // The next file in the folder (or first if is_dir)
} File;

/* The Filesystem struct
    A file system is a list of inodes, whith each node pointing to the address
    blocks in memory used by each file. */
typedef struct FileSystem {
    size_t size;                // Total filesystem memory space, in bytes
    Inode *head;                // Ptr to fs's head node / 1st mem block addr
    Inode *first_free;          // Ptr to the "free nodes" list. NEEDED?
    File *root;                 // Ptr to filesystem's root dir (a File obj)
} FileSystem;

// Memory block structure - adapted from clauter's in-class lecture (DF):
// struct __myfs_memory_block_t {  // MemBlock
//       size_t      size;
//       size_t      user_size;
//       __myfs_off_t next;
// } __myfs_memory_block_t;

// // Filesystem handle structure - adapted from clauter's in-lass lecture (DF):
// struct __myfs_handle_t {      // FSHandle
//       uint32_t    magic;
//       __myfs_off_t free_memory;
//       __myfs_off_t root dir;
//       size_t       size;
// } __myfs_handle_t;

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

