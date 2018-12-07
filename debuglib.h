/* A collection of debug functions for myfs.

   Author: Dustin Fast, 2018
*/

// Print filesystem's data structure sizes
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

// Print inode stats
void print_inode_debug(FSHandle *fs, Inode *inode) {
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

// Sets up files inside the filesystem for debugging purposes
void static init_files_debug(FSHandle *fs) {
    printf("DEBUG: Initializing test files/folders ");
    printf("/, /dir1, /dir1/file1 through /dir1/file4, and /file5\n");

    // Init test dirs/files
    Inode *dir1 = dir_new(fs, fs_rootnode_get(fs), "dir1");
    Inode *file1 = file_new(fs, "/dir1", "file1", "hello from file 1", 17);
    Inode *file2 = file_new(fs, "/dir1", "file2", "hello from file 2", 17);
    Inode *file3 = file_new(fs, "/dir1", "file3", "hello from file 3", 17);
    Inode *file4 = file_new(fs, "/dir1", "file4", "hello from file 4", 17);
    
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
    printf("\n");

    ////////////////////////////////////////////////////////////////////////
    // Display test file/directory attributes

    char file1_path[] =  "/dir1/file1";
    char file2_path[] =  "/dir1/file2";
    char file3_path[] =  "/dir1/file3";
    char file4_path[] =  "/dir1/file4";
    char file5_path[] =  "/file5";

    // Root dir
    printf("\nExamining / ");
    print_inode_debug(fs, fs_rootnode_get(fs));
    
    // Dir 1
    printf("\nExamining /dir1 ");
    print_inode_debug(fs, resolve_path(fs, "/dir1"));

    // File1
    printf("\nExamining /dir1/file1 ");
    print_inode_debug(fs, resolve_path(fs, file1_path));

    // File2
    printf("\nExamining /dir1/file2 ");
    print_inode_debug(fs, resolve_path(fs, "/dir1/file2"));

    // File 5 (commented out because multi-memblock data hogs screen)
    // printf("\nExamining /file5 ");
    // print_inode_debug(fs, resolve_path(fs, "/file5"));

    /////////////////////////////////////////////////////////////////////////
    // Begin 13 stub tests

    char *buf;
    int *errnoptr;
    size_t size, offset;
    int ret_val;

    char *path = file1_path;  // Path to test file

    // Test __myfs_read_implem
    size = 17;
    offset = 0;
    buf = malloc(size);
    ret_val = __myfs_read_implem(fsptr, fssize, errnoptr, path, buf, size, offset);

    printf("\nRECEIVED %d bytes from __myfs_read_implem():\n", ret_val); 
    write(fileno(stdout), buf, ret_val);
    printf("\n");

    free(buf);

    /////////////////////////////////////////////////////////////////////////
    // Cleanup
    
    printf("\n\nExiting...\n");
    free(fsptr);

    return 0; 
}
