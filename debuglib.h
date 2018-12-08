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
static void print_result_debug(char *title, int result, int expected) {
    printf("%s", title);
    if (result == expected)
        printf("PASS");
    else
        printf("FAIL");
    printf("\n");
}


// Sets up files inside the filesystem for debugging purposes
static void init_files_debug(FSHandle *fs) {
    printf("\n--- Initializing test files/folders ---");

    // Init test dirs/files
    Inode *dir1 = dir_new(fs, fs_rootnode_get(fs), "dir1");
    Inode *file1 = file_new(fs, "/dir1", "file1", "hello from file 1", 17);
    Inode *file2 = file_new(fs, "/dir1", "file2", "hello from file 2", 17);
    // Inode *file3 = file_new(fs, "/dir1", "file3", "hello from file 3", 17);
    // Inode *file4 = file_new(fs, "/dir1", "file4", "hello from file 4", 17);
    
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
    Inode *file5 = file_new(fs, "/", "file5", lg_data, data_sz);
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
    // print_fs_debug(fs);      // Display fs properties

    init_files_debug(fs);       // Init test files and dirs for debugging

    ////////////////////////////////////////////////////////////////////////
    // Display test file/directory attributes

    char path_root[] =  "/";
    char path_dir1[] =  "/dir1";
    char path_file1[] =  "/dir1/file1";
    char path_file2[] =  "/dir1/file2";
    char path_file3[] =  "/dir1/file3";
    char path_file4[] =  "/dir1/file4";
    char path_file5[] =  "/file5";

    // Root dir
    printf("\nExamining / ");
    print_inode_debug(fs, fs_rootnode_get(fs));
    
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

    char *filepath = path_file2;  // Test file path
    char *dirpath = path_dir1;    // Test directory path
    char nofilepath[] = "/filethatdoesntexist";
    char badpath[] = "badpath";
    char newfilepath[] = "/newfile1";
    char newdirpath[] = "/newdir1";

    char *buf;
    struct stat *stbuf = malloc(sizeof(struct stat));
    struct statvfs *stvbuf = malloc(sizeof(struct statvfs));

    int e;
    int result;


    // getattr
    char getattr0[] = "getattr_implem(SUCCESS):\n";
    result = __myfs_getattr_implem(fsptr, fssize, &e, 0, 0, filepath, stbuf);
    free(stbuf);
    print_result_debug(getattr0, result, 0);

    char getattr1[] = "getattr_implem(FAIL/NOEXIST):\n";
    result = __myfs_getattr_implem(fsptr, fssize, &e, 0, 0, nofilepath, stbuf);
    free(stbuf);
    print_result_debug(getattr1, result, -1);

    char getattr2[] = "getattr_implem(FAIL/BADPATH):\n";
    result = __myfs_getattr_implem(fsptr, fssize, &e, 0, 0, badpath, stbuf);
    free(stbuf);
    print_result_debug(getattr2, result, -1);


    // mknod
    char mknod0[] = "mknod_implem(SUCCESS):\n";
    result = __myfs_mknod_implem(fsptr, fssize, &e, newfilepath);
    print_result_debug(mknod0, result, 1);

    char mknod1[] = "mknod_implem(FAIL/ALREADY EXISTS):\n";
    result = __myfs_mknod_implem(fsptr, fssize, &e, filepath);
    print_result_debug(mknod1, result, -1);

    char mknod2[] = "mknod_implem(FAIL/BADPATH):\n";
    result = __myfs_mknod_implem(fsptr, fssize, &e, badpath);
    print_result_debug(mknod2, result, -1);


    // mkdir
    char mkdir0[] = "mkdir_implem(SUCCESS):\n";
    result = __myfs_mkdir_implem(fsptr, fssize, &e, newdirpath);
    print_result_debug(mkdir0, result, 0);

    char mkdir1[] = "mkdir_implem(FAIL/ALREADY EXISTS):\n";
    result = __myfs_mkdir_implem(fsptr, fssize, &e, dirpath);
    print_result_debug(mkdir1, result, -1);

    char mkdir2[] = "mkdir_implem(FAIL/BADPATH):\n";
    result = __myfs_mkdir_implem(fsptr, fssize, &e, dirpath);
    print_result_debug(mkdir2, result, -1);


    // rmdir
    char rmdir0[]= "rmdir_implem(SUCCESS):\n";
    // result = __myfs_rmdir_implem(fsptr, fssize, &e, newdirpath);
    // print_result_debug(rmdir0, result, 0);

    char rmdir1[] = "rmdir_implem(FAIL/NONEMPTY):\n";
    result = __myfs_rmdir_implem(fsptr, fssize, &e, dirpath);
    print_result_debug(rmdir1, result, -1);

    char rmdir2[] = "rmdir_implem(FAIL/BADPATH):\n";
    result = __myfs_rmdir_implem(fsptr, fssize, &e, badpath);
    print_result_debug(rmdir2, result, -1);

    char rmdir3[] = "rmdir_implem(FAIL/ISNOTDIR):\n";
    result = __myfs_rmdir_implem(fsptr, fssize, &e, filepath);
    print_result_debug(rmdir3, result, -1);


    // utims
    char utims0[] = "utims_implem(SUCCESS):\n";
    const struct timespec ts[2];
    result = __myfs_utimens_implem(fsptr, fssize, &e, filepath, ts);
    print_result_debug(utims0, result, 0);

    char utims1[] = "utims_implem(FAIL/BADPATH):\n";
    result = __myfs_utimens_implem(fsptr, fssize, &e, badpath, ts);
    print_result_debug(utims1, result, -1);

    
    // statfs
    result = __myfs_statfs_implem(fsptr, fssize, &e, stvbuf);
    print_result_debug("statfs_implem(SUCCESS):\n", result, 0);


    // truncate
    char truncate0[] = "truncate_implem(SUCCESS):\n";
    result = __myfs_truncate_implem(fsptr, fssize, &e, filepath, 5);
    print_result_debug(truncate0, result, 0);

    char truncate1[] = "truncate_implem(FAIL/NOEXSIST):\n";
    result = __myfs_truncate_implem(fsptr, fssize, &e, nofilepath, 5);
    print_result_debug(truncate1, result, -1);

    char truncate2[] = "truncate_implem(FAIL/BADPATH):\n";
    result = __myfs_truncate_implem(fsptr, fssize, &e, badpath, 5);
    print_result_debug(truncate2, result, 0);


    // open
    char open0[] = "open_implem(SUCCESS):\n";
    result = __myfs_open_implem(fsptr, fssize, &e, filepath);
    print_result_debug(open0, result, 17);


    // read
    printf("read_implem('Hello from file 2'):\n");
    buf = malloc(17);
    result = __myfs_read_implem(fsptr, fssize, &e, filepath, buf, 17, 0);
    printf("(%d bytes read)\n", result); 
    write(fileno(stdout), buf, result);
    free(buf);


    // write
    printf("\nwrite_implem(SUCCESS):\n");
    result = __myfs_write_implem(
        fsptr, fssize, &e, filepath, "test write", 10, 11);
    printf("(%d bytes written)\n", result); 


    //// TODO: __myfs_readdir_implem

    //// TODO: __myfs_rename_implem


    // print_inode_debug(fs, resolve_path(fs, filepath));

    /////////////////////////////////////////////////////////////////////////
    // Cleanup
    
    printf("\n\nExiting...\n");
    free(fsptr);

    return 0; 
}
