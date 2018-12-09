/* A collection of debug functions for myfs.

   Author: Dustin Fast, 2018
*/

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
    printf("File system properties: \n");
    printf("    fs (fsptr)      : %lu\n", (lui)fs);
    printf("    fs->num_inodes  : %lu\n", (lui)fs->num_inodes);
    printf("    fs->num_memblks : %lu\n", (lui)fs->num_memblocks);
    printf("    fs->size_b      : %lu (%lu kb)\n", fs->size_b, bytes_to_kb(fs->size_b));
    printf("    fs->inode_seg   : %lu\n", (lui)fs->inode_seg);
    printf("    fs->mem_seg     : %lu\n", (lui)fs->mem_seg);
    printf("    Free Inodes     : %lu\n", inodes_numfree(fs));
    printf("    Num Memblocks   : %lu\n", memblocks_numfree(fs));
    printf("    Free space      : %lu bytes (%lu kb)\n", fs_freespace(fs), bytes_to_kb(fs_freespace(fs)));
}

// Print inode stats
static void print_inode_debug(FSHandle *fs, Inode *inode) {
    if (inode == NULL) printf("    FAIL: inode is NULL.\n");

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
    printf("      data           : %s\n",  (char*)(memhead + ST_SZ_MEMHEAD));
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

// Helper to resolve pathand return data for a file
static size_t debug_file_data_get(FSHandle *fs, const char *path, char *buf) {
    Inode *inode = resolve_path(fs, path);
    return inode_data_get(fs, inode, buf);
}

// Sets up files inside the filesystem for debugging purposes
static void init_files_debug(FSHandle *fs) {
    printf("\n--- Initializing test files/folders ---");

    // Init test dirs/files
    dir_new(fs, fs_rootnode_get(fs), "dir1");
    file_new(fs, "/dir1", "file1", "hello from file 1", 17);
    file_new(fs, "/dir1", "file2", "hello from file 2", 17);
    // file_new(fs, "/dir1", "file3", "hello from file 3", 17);
    // file_new(fs, "/dir1", "file4", "hello from file 4", 17);
    
    // Init file 5 consisting of a lg string of a's & b's & terminated w/ 'c'.
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
    // Begin test filesys init  

    size_t fssize = kb_to_bytes(32) + ST_SZ_FSHANDLE;

    // Allocate fs space and associate with a filesys handle
    void *fsptr = malloc(fssize); 
    FSHandle *fs = fs_init(fsptr, fssize);

    printf("\n");
    print_fs_debug(fs);      // Display fs properties

    init_files_debug(fs);       // Init test files and dirs for debugging

    ////////////////////////////////////////////////////////////////////////
    // Display test file/directory attributes

    char path_root[] =  "/";
    char path_dir1[] =  "/dir1";
    char path_file1[] =  "/dir1/file1";
    char path_file2[] =  "/dir1/file2";

    // Root dir
    printf("\nExamining / ");
    print_inode_debug(fs, resolve_path(fs, path_root));
    
    // Dir 1
    printf("\nExamining /dir1 ");
    print_inode_debug(fs, resolve_path(fs, path_dir1));

    // File 1
    printf("\nExamining /dir1/file1 ");
    print_inode_debug(fs, resolve_path(fs, path_file1));

    // File 5 (commented out because multi-memblock data hogs screen)
    // printf("\nExamining /file5 ");
    // print_inode_debug(fs, resolve_path(fs, path_file5));

    printf("\n");

    /////////////////////////////////////////////////////////////////////////
    // Begin 13 func tests
    printf("\n--- Testing __myfs_implem functions ---\n");

    // Test paths
    char *filepath = path_file2;
    char *dirpath = path_dir1;
    char nofilepath[] = "/filethatdoesntexist";
    char badpath[] = "badpath";
    char newfilepath[] = "/newfile1";
    char newdirpath[] = "/newdir1";

    // Results containers
    int e;
    int r;
    char *buf;
    size_t sz;
    const struct timespec ts[2];
    struct stat stbuf[1];
    struct statvfs stvbuf[1];


    // getattr
    r = __myfs_getattr_implem(fsptr, fssize, &e, 0, 0, filepath, stbuf);
    print_result_debug("getattr_implem(SUCCESS):\n", r, 0);

    r = __myfs_getattr_implem(fsptr, fssize, &e, 0, 0, nofilepath, stbuf);
    print_result_debug( "getattr_implem(FAIL/NOEXIST):\n", r, -1);

    r = __myfs_getattr_implem(fsptr, fssize, &e, 0, 0, badpath, stbuf);
    print_result_debug("getattr_implem(FAIL/BADPATH):\n", r, -1);


    // mknod
    r = __myfs_mknod_implem(fsptr, fssize, &e, newfilepath);
    print_result_debug("mknod_implem(SUCCESS):\n", r, 0);

    r = __myfs_mknod_implem(fsptr, fssize, &e, filepath);
    print_result_debug("mknod_implem(FAIL/ALREADY EXISTS):\n", r, -1);

    r = __myfs_mknod_implem(fsptr, fssize, &e, badpath);
    print_result_debug("mknod_implem(FAIL/BADPATH):\n", r, -1);


    // mkdir
    r = __myfs_mkdir_implem(fsptr, fssize, &e, newdirpath);
    print_result_debug("mkdir_implem(SUCCESS):\n", r, 0);

    r = __myfs_mkdir_implem(fsptr, fssize, &e, dirpath);
    print_result_debug("mkdir_implem(FAIL/BADPATH):\n", r, -1);

    r = __myfs_mkdir_implem(fsptr, fssize, &e, dirpath);
    print_result_debug("mkdir_implem(FAIL/BADPATH):\n", r, -1);


    // rmdir
    r = __myfs_rmdir_implem(fsptr, fssize, &e, newdirpath);
    print_result_debug("rmdir_implem(SUCCESS):\n", r, 0);

    r = __myfs_rmdir_implem(fsptr, fssize, &e, dirpath);
    print_result_debug("rmdir_implem(FAIL/NONEMPTY):\n", r, -1);

    r = __myfs_rmdir_implem(fsptr, fssize, &e, badpath);
    print_result_debug("rmdir_implem(FAIL/BADPATH):\n", r, -1);

    r = __myfs_rmdir_implem(fsptr, fssize, &e, filepath);
    print_result_debug("rmdir_implem(FAIL/ISNOTDIR):\n", r, -1);


    // utims
    r = __myfs_utimens_implem(fsptr, fssize, &e, filepath, ts);
    print_result_debug("utims_implem(SUCCESS):\n", r, 0);

    r = __myfs_utimens_implem(fsptr, fssize, &e, badpath, ts);
    print_result_debug("utims_implem(FAIL/BADPATH):\n", r, -1);

    
    // statfs
    r = __myfs_statfs_implem(fsptr, fssize, &e, stvbuf);
    print_result_debug("statfs_implem(SUCCESS):\n", r, 0);


    // open
    r = __myfs_open_implem(fsptr, fssize, &e, filepath);
    print_result_debug("open_implem(SUCCESS):\n", r, 0);

    r = __myfs_open_implem(fsptr, fssize, &e, nofilepath);
    print_result_debug("open_implem(FAIL/NOEXIST):\n", r, -1);

    // readdir_implem
    char ***namesptr = NULL;
    r = __myfs_readdir_implem(fsptr, fssize, &e, filepath, namesptr);
    print_result_debug("readdir_implem(FAIL/NOTDIR):\n", r, -1);

    r = __myfs_readdir_implem(fsptr, fssize, &e, dirpath, namesptr);
    print_result_debug("readdir_implem('file1, file2'):\n", r, 2);


    // read
    printf("\nread_implem('Hello from file 2'):\n");
    buf = malloc(17);
    r = __myfs_read_implem(fsptr, fssize, &e, filepath, buf, 17, 0);
    printf("(%d bytes read)\n", r); 
    write(fileno(stdout), buf, r);
    free(buf);


    // write
    printf("\n\nwrite_implem('Hello from test write'):\n");
    r = __myfs_write_implem(
        fsptr, fssize, &e, filepath, "test write", 10, 11);
    printf("(%d bytes written)\n", r); 
    buf = malloc(1);
    sz = debug_file_data_get(fs, filepath, buf);
    write(fileno(stdout), buf, sz);
    free(buf);

    
    // truncate
    printf("\n\ntruncate_implem('hello'):\n");
    r = __myfs_truncate_implem(fsptr, fssize, &e, filepath, 5);
    buf = malloc(1);
    sz = debug_file_data_get(fs, filepath, buf);
    write(fileno(stdout), buf, sz);
    free(buf);

    // rename
    // TODO: Not working
    // char rename1[] = "\nrename_implem(SUCCESS):\n";
    // r = __myfs_rename_implem(fsptr, fssize, &e, "/dir1/file1", "/file1");
    // print_result_debug(rename1, r, 0);


    // print_inode_debug(fs, fs_rootnode_get(fs));

    // print_inode_debug(fs, resolve_path(fs, "/"));

    /////////////////////////////////////////////////////////////////////////
    // Cleanup
    
    printf("\n\nExiting...\n");
    free(fsptr);

    return 0; 
}
