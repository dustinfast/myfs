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

// Prints either a "Success" or "Fail" to the console based on the given int.
static void print_result_debug(int result) {
    if (!result)
        printf("Success");
    else
        printf("Fail");
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
    print_fs_debug(fs);     // Display fs properties

    init_files_debug(fs);   // Init test files/dirs

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

    // File1
    printf("\nExamining /dir1/file1 ");
    print_inode_debug(fs, resolve_path(fs, path_file1));

    // File2
    printf("\nExamining /dir1/file2 ");
    print_inode_debug(fs, resolve_path(fs, path_file2));

    // File 5 (commented out because multi-memblock data hogs screen)
    // printf("\nExamining /file5 ");
    // print_inode_debug(fs, resolve_path(fs, path_file5));

    printf("\n");

    /////////////////////////////////////////////////////////////////////////
    // Begin 13 stub tests
    printf("\n--- Testing __myfs_implem functions ---\n");

    char *filepath = path_file2;  // Test file path
    char *dirpath = path_dir1;    // Test directory path
    char nofilepath[] = "/filethatdoesntexist";
    char badpath1[] = "badpath";
    char badpath2[] = "/fil:e";
    char newfilepath[] = "/newfile1";
    char newdirpath[] = "/newdir1";

    char *buf;
    struct stat *stbuf = malloc(sizeof(struct stat));
    size_t size, offset;
    int errnoptr;
    int result;

    // __myfs_getattr_implem, for existing file
    // printf("\n__myfs_getattr_implem(SUCCESS):\n");
    // result = __myfs_getattr_implem(fsptr, fssize, &errnoptr, 0, 0, filepath, stbuf);
    // free(stbuf);
    // print_result_debug(result);

    // // __myfs_getattr_implem, for non-existent file
    // printf("\n__myfs_getattr_implem(FAIL):\n");
    // result = __myfs_getattr_implem(fsptr, fssize, &errnoptr, 0, 0, nofilepath, stbuf);
    // free(stbuf);
    // print_result_debug(result);

    // //// TODO: __myfs_readdir_implem

    // // __myfs_mknod_implem, for non-existent file
    // printf("\n__myfs_mknod_implem(SUCCESS):\n");
    // result = __myfs_mknod_implem(fsptr, fssize, &errnoptr, newfilepath);
    // print_result_debug(result);

    // // __myfs_mknod_implem, where file already exists
    // printf("\n__myfs_mknod_implem(FAIL):\n");
    // result = __myfs_mknod_implem(fsptr, fssize, &errnoptr, filepath);
    // print_result_debug(result);

    // //// TODO: __myfs_unlink_implem

    __myfs_mkdir_implem(fsptr, fssize, &errnoptr, "/td1");
    Inode *t = resolve_path(fs, "/td1");
    print_inode_debug(fs, t);

    // __myfs_rmdir_implem for non-empty dir
    printf("\n__myfs_rmdir_implem(SUCCESS):\n");
    result = __myfs_rmdir_implem(fsptr, fssize, &errnoptr, "/td1");
    print_result_debug(result);

    // __myfs_rmdir_implem for non-empty dir
    printf("\n__myfs_rmdir_implem(FAIL):\n");
    result = __myfs_rmdir_implem(fsptr, fssize, &errnoptr, dirpath);
    print_result_debug(result);



    // __myfs_mkdir_implem, for non-existent dir
    printf("\n__myfs_mkdir_implem(SUCCESS):\n");
    result = __myfs_mkdir_implem(fsptr, fssize, &errnoptr, newdirpath);
    print_result_debug(result);

    // __myfs_mkdir_implem, where dir already exists
    printf("\n__myfs_mkdir_implem(FAIL):\n");
    result = __myfs_mkdir_implem(fsptr, fssize, &errnoptr, badpath1);
    print_result_debug(result);
    
    //// TODO: __myfs_rename_implem

    // __myfs_truncate_implem
    printf("\n__myfs_truncate_implem(SUCCESS):\n");
    offset = 5;
    result = __myfs_truncate_implem(fsptr, fssize, &errnoptr, filepath, offset);
    print_result_debug(result);

    //// __myfs_open_implem
    printf("\n__myfs_open_implem(SUCCESS):\n");
    result = __myfs_open_implem(fsptr, fssize, &errnoptr, "/file5");
    print_result_debug(result);

    //// __myfs_read_implem
    printf("\n__myfs_read_implem(SUCCESS):\n");
    size = 20;
    offset = 0;
    buf = malloc(size);
    result = __myfs_read_implem(fsptr, fssize, &errnoptr, filepath, buf, size, offset);

    printf("(%d bytes read)\n", result); 
    write(fileno(stdout), buf, result);
    free(buf);
    printf("\n");


    // // __myfs_read_implem after truncate
    // printf("\n__myfs_read_implem(T):\n");
    // result = __myfs_truncate_implem(fsptr, fssize, &errnoptr, filepath, 2);
    // size = 20;
    // offset = 0;
    // buf = malloc(size);
    // result = __myfs_read_implem(fsptr, fssize, &errnoptr, filepath, buf, size, offset);

    // printf("(%d bytes read)\n", result); 
    // write(fileno(stdout), buf, result);
    // free(buf);
    // printf("\n");

    // print_inode_debug(fs, resolve_path(fs, filepath));


    //// __myfs_write_implem
    printf("\n__myfs_write_implem(SUCCESS):\n");
    char data[10] = "test write";
    size = 10;
    offset = 11;
    result = __myfs_write_implem(fsptr, fssize, &errnoptr, filepath, data, size, offset);

    printf("(%d bytes written)\n", result); 
    write(fileno(stdout), data, result);
    printf("\n");

    //// __myfs_utimens_implem
    printf("\n__myfs_utims_implem(SUCCESS):\n");
    const struct timespec ts[2];
    result = __myfs_utimens_implem(fsptr, fssize, &errnoptr, filepath, ts);
    print_result_debug(result);
    
    //// TODO: __myfs_statfs_implem
    

    // print_inode_debug(fs, resolve_path(fs, filepath));

    /////////////////////////////////////////////////////////////////////////
    // Cleanup
    
    printf("\n\nExiting...\n");
    free(fsptr);

    return 0; 
}
