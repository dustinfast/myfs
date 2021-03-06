/* A collection of debug/test functoins for myfs.

   Author: Dustin Fast, 2018
*/

// Returns number of free bytes in the fs, as based on num free mem blocks.
static size_t fs_freespace_debug(FSHandle *fs) {
    size_t num_memblocks = memblocks_numfree(fs);
    return num_memblocks * DATAFIELD_SZ_B;
}

// Returns the number of free inodes in the filesystem
static size_t inodes_numfree_debug(FSHandle *fs) {
    Inode *inode = fs->inode_seg;
    size_t num_inodes = fs->num_inodes;
    size_t num_free = 0;

    for (int i = 0; i < num_inodes; i++) {
        if (inode_isfree(inode))
            num_free++;
        inode++;
    }
    return num_free;
}

// Print filesystem's data structure sizes
static void print_struct_debug() {
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
static void print_fs_debug(FSHandle *fs) {
    printf("\nFile system properties: \n");
    printf("    fs (fsptr)      : %lu\n", (lui)fs);
    printf("    fs->num_inodes  : %lu\n", (lui)fs->num_inodes);
    printf("    fs->num_memblks : %lu\n", (lui)fs->num_memblocks);
    printf("    fs->size_b      : %lu (%lu kb)\n", fs->size_b, 
        bytes_to_kb(fs->size_b));
    printf("    fs->inode_seg   : %lu\n", (lui)fs->inode_seg);
    printf("    fs->mem_seg     : %lu\n", (lui)fs->mem_seg);
    printf("    Free Inodes     : %lu\n", inodes_numfree_debug(fs));
    printf("    Num Memblocks   : %lu\n", memblocks_numfree(fs));
    printf("    Free space      : %lu bytes (%lu kb)\n", 
        fs_freespace_debug(fs), bytes_to_kb(fs_freespace_debug(fs)));
}

// Prints an inode's properties
static void print_inode_debug(FSHandle *fs, Inode *inode) {
    if (inode == NULL) {
        printf("    FAIL: inode is NULL.\n");
        return;
    }

    printf("Inode -\n");
    printf("   addr                : %lu\n", (lui)inode);
    printf("   offset              : %lu\n", (lui)offset_from_ptr(fs, inode));
    printf("   name                : %s\n", inode->name);
    printf("   is_dir              : %lu\n", (lui)inode->is_dir);
    printf("   subdirs             : %lu\n", (lui)inode->subdirs);
    printf("   file_size_b         : %lu\n", (lui)inode->file_size_b);
    printf("   last_acc            : %09ld\n", inode->last_acc->tv_sec);
    printf("   last_mod            : %09ld\n", inode->last_mod->tv_sec);
    printf("   offset_firstblk     : %lu\n", (lui)inode->offset_firstblk);  

    MemHead *memhead = inode_firstmemblock(fs, inode);
    printf("   first mem block -\n");
    printf("      addr           : %lu\n", (lui)memhead);
    printf("      offset         : %lu\n", (lui)offset_from_ptr(fs, memhead));
    printf("      not_free       : %lu\n", (lui)memhead->not_free);
    printf("      data_size_b    : %lu\n", (lui)memhead->data_size_b);
    printf("      offset_nextblk : %lu\n", (lui)memhead->offset_nextblk);
    printf("      data           :\n");
    printf("'%s'\n",  (char*)(memhead + ST_SZ_MEMHEAD));
}

// Prints either a PASS or FAIL to the console based on the given params
static void print_result_debug(char *title, int r, int expected) {
    printf("%s", title);
    if (r == expected)
        printf("PASS");
    else
        printf("FAIL");
    printf("\n");
}

// Helper to resolve path and return data for a file
static size_t debug_file_data_get(FSHandle *fs, const char *path, char *buf) {
    Inode *inode = resolve_path(fs, path);
    return inode_data_get(fs, inode, buf);
}

// Sets up files inside the filesystem for debugging purposes
static void init_files_debug(FSHandle *fs) {
    printf("\n--- Initializing test files/folders ---");

    // Init dir1 test files
    Inode *dir1 = dir_new(fs, fs_rootnode_get(fs), "dir1");
    file_new(fs, "/dir1", "file1", "hello from file 1", 17);
    file_new(fs, "/dir1", "file2", "hello from file 2", 17);
    
    // Init dir2 test files
    dir_new(fs, dir1, "dir2");
    file_new(fs, "/dir2", "file3", "hello from file 3", 17);
    
    // Init /file5, consisting of a lg string of a's & b's & terminated w/ 'c'.
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
    file_new(fs, "/", "file5", lg_data, data_sz);
}


int main() 
{
    printf("------------- File System Test Space -------------\n");
    printf("--------------------------------------------------\n\n");
    print_struct_debug();
      
    /////////////////////////////////////////////////////////////////////////
    // Init a fs for testing purposes

    // Allocate fs space and associate with a filesys handle
    size_t fssize = kb_to_bytes(128) + ST_SZ_FSHANDLE;
    void *fsptr = malloc(fssize); 
    FSHandle *fs = fs_init(fsptr, fssize);

    print_fs_debug(fs);      // Display fs properties
    init_files_debug(fs);    // Init test files/dirs

    ////////////////////////////////////////////////////////////////////////
    // Display a sample of the test files attributes

    // Root dir
    printf("\nExamining / ");
    print_inode_debug(fs, resolve_path(fs, "/"));
    
    // Dir 1
    printf("\nExamining /dir1 ");
    print_inode_debug(fs, resolve_path(fs, "/dir1"));

    // File 1
    printf("\nExamining /dir1/file1 ");
    print_inode_debug(fs, resolve_path(fs, "/dir1/file1"));

    printf("\n");

    /////////////////////////////////////////////////////////////////////////
    // Begin 13 func tests
    printf("\n--- Testing __myfs_implem functions ---\n");

    // Test paths
    char *filepath = "/dir1/file1";
    char *dirpath = "/dir1";
    char nofilepath[] = "/filethatdoesntexist";
    char badpath[] = "badpath";
    char newfilepath[] = "/newfile1";
    char newdirpath[] = "/newdir1";

    // Shared results containers
    char *buf;
    int e;
    int r;

    // getattr
    struct stat stbuf;
    r = __myfs_getattr_implem(fsptr, fssize, &e, 0, 0, filepath, &stbuf);
    print_result_debug("getattr_implem(SUCCESS):\n", r, 0);

    r = __myfs_getattr_implem(fsptr, fssize, &e, 0, 0, nofilepath, &stbuf);
    print_result_debug( "getattr_implem(FAIL/NOEXIST):\n", r, -1);

    // mknod
    r = __myfs_mknod_implem(fsptr, fssize, &e, newfilepath);
    print_result_debug("mknod_implem(SUCCESS):\n", r, 0);

    r = __myfs_mknod_implem(fsptr, fssize, &e, newfilepath);
    print_result_debug("mknod_implem(FAIL/EXISTS):\n", r, -1);

    // unlink
    r = __myfs_unlink_implem(fsptr, fssize, &e, newfilepath);
    print_result_debug("unlink_implem(SUCCESS):\n", r, 0);
    
    r = __myfs_unlink_implem(fsptr, fssize, &e, newfilepath);
    print_result_debug("unlink_implem(FAIL/NOEXIST):\n", r, -1);

    // mkdir
    r = __myfs_mkdir_implem(fsptr, fssize, &e, newdirpath);
    print_result_debug("mkdir_implem(SUCCESS):\n", r, 0);

    r = __myfs_mkdir_implem(fsptr, fssize, &e, newdirpath);
    print_result_debug("mkdir_implem(FAIL/EXISTS):\n", r, -1);

    // rmdir
    r = __myfs_rmdir_implem(fsptr, fssize, &e, newdirpath);
    print_result_debug("rmdir_implem(SUCCESS):\n", r, 0);

    r = __myfs_rmdir_implem(fsptr, fssize, &e, dirpath);
    print_result_debug("rmdir_implem(FAIL/NOTEMPTY):\n", r, -1);

    r = __myfs_rmdir_implem(fsptr, fssize, &e, filepath);
    print_result_debug("rmdir_implem(FAIL/ISNOTDIR):\n", r, -1);

    // utims
    const struct timespec ts[2];
    r = __myfs_utimens_implem(fsptr, fssize, &e, filepath, ts);
    print_result_debug("utims_implem(SUCCESS):\n", r, 0);

    r = __myfs_utimens_implem(fsptr, fssize, &e, badpath, ts);
    print_result_debug("utims_implem(FAIL/BADPATH):\n", r, -1);
    
    // statfs
    struct statvfs stvbuf;
    r = __myfs_statfs_implem(fsptr, fssize, &e, &stvbuf);
    print_result_debug("statfs_implem(SUCCESS):\n", r, 0);

    // open
    r = __myfs_open_implem(fsptr, fssize, &e, filepath);
    print_result_debug("open_implem(SUCCESS):\n", r, 0);

    r = __myfs_open_implem(fsptr, fssize, &e, nofilepath);
    print_result_debug("open_implem(FAIL/NOEXIST):\n", r, -1);

    // readdir_implem
    char **namesptr;
    r = __myfs_readdir_implem(fsptr, fssize, &e, dirpath, &namesptr);
    print_result_debug("readdir_implem('file1, file2, dir2'):\n", r, 3);

    r = __myfs_readdir_implem(fsptr, fssize, &e, filepath, &namesptr);
    print_result_debug("readdir_implem(FAIL/ISNOTDIR):\n", r, -1);
    free(namesptr);

    // rename (file)
    r = __myfs_rename_implem(fsptr, fssize, &e, "/dir1/file2", "/file2");
    print_result_debug("rename_implem(FILE-SUCCESS):\n", r, 0);

    // rename (dir)
    r = __myfs_rename_implem(fsptr, fssize, &e, "/dir1/dir2", "/dir2");
    print_result_debug("rename_implem(DIREMPTY-SUCCESS):\n", r, 0);

    r = __myfs_rename_implem(fsptr, fssize, &e, "/dir2", "/dir1/dir2");
    print_result_debug("rename_implem(DIRNOTEMPTY-SUCCESS):\n", r, 0);

    // read
    printf("read_implem('hello from file 2'):\n");
    buf = malloc(17);
    r = __myfs_read_implem(fsptr, fssize, &e, filepath, buf, 17, 0);
    (memcmp(buf, "hello from file 1", 17) == 0) ? printf("PASS") : printf("FAIL");    
    free(buf);

    // write
    printf("\nwrite_implem('hello from test write'):\n");
    r = __myfs_write_implem(
        fsptr, fssize, &e, filepath, "test write", 10, 11);
    buf = malloc(1);
    debug_file_data_get(fs, filepath, buf);
    (memcmp(buf, "hello from test write", 21) == 0) ? printf("PASS") : printf("FAIL");
    free(buf);

    // truncate
    printf("\ntruncate_implem('hello'):\n");
    r = __myfs_truncate_implem(fsptr, fssize, &e, filepath, 5);
    buf = malloc(1);
    debug_file_data_get(fs, filepath, buf);
    (memcmp(buf, "hello", 5) == 0) ? printf("PASS") : printf("FAIL");
    free(buf);


    /////////////////////////////////////////////////////////////////////////
    // Cleanup
    
    printf("\n\nExiting...\n");
    free(fsptr);

    return 0; 
}
