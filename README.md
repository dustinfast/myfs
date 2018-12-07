# a4


## Contributions

__Dustin Fast__: implementation.c and implementation.h
__Joel Keller__: implementation.c
__Christoph Lauter__: myfs.c and implementation.c skeleton
__Brooks Woods__: implementation.c

## File Hierarchy
* **implemnetation.c**: Filesystem emulations and helper functions
* **implementation.h**: File system struct defs and generic helper functions
* **myfs.c**: FUSE interface and helper functions

### The filesystem:

+ File system structure in memory is:
     _ _ _ _ _ _ _ _ _______________________ ___________________________
    |   FSHandle    |       Inodes          |       Memory Blocks       | 
    |_ _ _ _ _ _ _ _|_______________________|___________________________|
    ^               ^                       ^
    fsptr           Inodes Segment          Memory Blocks segment
                    (0th is root inode)     (0th is root dir memory block)

    + Memory blocks look like:
     ______________ ________________________
    |   MemHead    |         Data           |
    |______________|________________________|


    Directory file data field structure:
        Ex: "dir1:offset\ndir2:offset\nfile1:offset"
        Ex: "file1:offset\nfile2:offset"

    Design Decisions:
        Assume a single process accesses the fs at a time?
        To begin writing data before checking fs has enough room?
        Assume only absolute paths passed to 13 funcs?
        Filename and path chars to allow.
        Simple design vs. Better design.

## Assignment Instructions

### The File system must not:

* Segfault
* Fail with exit(1) in case of an error.
* Depend on memory outside of the filesystem memory regionf
* Use any global variables - use a struct containing all "global" data at
the start of the memory region representing the filesystem.
*Store any pointer directly to mem pointed to by fsptr; instead store 
offsets from the start of the mem region.

### The File system must:

* Must Run in memory (memory comes from mmap).
* Must support all the 13 operations stubbed out below.
* Must support access and modification times and statfs info.
Needs not support: access rights, links, symbolic links. 
* Is of size fssize pointed to by fsptr (Size of memory region >= 2048)
* On mounted for the first time, the whole memory region of size fssize
    pointed to by fsptr reads as zero-bytes. 
* On unmounted, memory is written to backup-file. 
* On mounted from backup, memory previously written may be non-zero bytes. 
    (only writes whats inside virtual memory address space to backup-file.) 
* On mount from backup-file, same data appears at the newly mapped vaddress. 

* May use libc functions, ex: malloc, calloc, free, strdup, strlen, 
strncpy, strchr, strrchr, memset, memcpy. 
      
### Professor's Advice:     
   * You MUST NOT store (the value of) pointers into the memory region
     that represents the filesystem. Pointers are virtual memory
     addresses and these addresses are ephemeral. Everything will seem
     okay UNTIL you remount the filesystem again.
  
     You may store offsets/indices (of type size_t) into the
     filesystem. These offsets/indices are like pointers: instead of
     storing the pointer, you store how far it is away from the start of
     the memory region. You may want to define a type for your offsets
     and to write two functions that can convert from pointers to
     offsets and vice versa.

   * Your FUSE process, which implements the filesystem, MUST NOT leak
     memory. Be careful in particular not to leak tiny amounts of memory that
     accumulate over time. A FUSE process is supposed to run for a long time!
     
     Check for memory leaks by running the FUSE process inside valgrind:
     valgrind --leak-check=full ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f


*IT IS REASONABLE TO PROCEED IN THE FOLLOWING WAY:*

   (1)   Design and implement a mechanism that initializes a filesystem
         whenever the memory space is fresh. That mechanism can be
         implemented in the form of a filesystem handle into which the
         filesystem raw memory pointer and sizes are translated.

   (2)   Design/implement functions to find and allocate free memory
         regions inside the filesystem memory space. There need to be 
         functions to free these regions again, too. Any "global" variable
         goes into the handle structure the mechanism designed at step (1) 
         provides.

   (3)   Carefully design a data structure able to represent all the
         pieces of information that are needed for files and
         (sub-)directories.  You need to store the location of the
         root directory in a "global" variable that, again, goes into the 
         handle designed at step (1).
          
   (4)   Write __myfs_getattr_implem and debug it thoroughly, as best as
         you can with a filesystem that is reduced to one
         function. Writing this function will make you write helper
         functions to traverse paths, following the appropriate
         subdirectories inside the file system. Strive for modularity for
         these filesystem traversal functions.

   (5)   Design and implement __myfs_readdir_implem. You cannot test it
         besides by listing your root directory with ls -la and looking
         at the date of last access/modification of the directory (.). 
         Be sure to understand the signature of that function and use
         caution not to provoke segfaults nor to leak memory.

   (6)   Design and implement __myfs_mknod_implem. You can now touch files 
         with 

         touch foo

         and check that they start to exist (with the appropriate
         access/modification times) with ls -la.

   (7)   Design and implement __myfs_mkdir_implem. Test as above.

   (8)   Design and implement __myfs_truncate_implem. You can now 
         create files filled with zeros:

         truncate -s 1024 foo

   (9)   Design and implement __myfs_statfs_implem. Test by running
         df before and after the truncation of a file to various lengths. 
         The free "disk" space must change accordingly.

   (10)  Design, implement and test __myfs_utimens_implem. You can now 
         touch files at different dates (in the past, in the future).

   (11)  Design and implement __myfs_open_implem. The function can 
         only be tested once __myfs_read_implem and __myfs_write_implem are
         implemented.

   (12)  Design, implement and test __myfs_read_implem and
         __myfs_write_implem. You can now write to files and read the data 
         back:

         echo "Hello world" > foo
         echo "Hallo ihr da" >> foo
         cat foo

         Be sure to test the case when you unmount and remount the
         filesystem: the files must still be there, contain the same
         information and have the same access and/or modification
         times.

   (13)  Design, implement and test __myfs_unlink_implem. You can now
         remove files.

   (14)  Design, implement and test __myfs_unlink_implem. You can now
         remove directories.

   (15)  Design, implement and test __myfs_rename_implem. This function
         is extremely complicated to implement. Be sure to cover all 
         cases that are documented in man 2 rename. The case when the 
         new path exists already is really hard to implement. Be sure to 
         never leave the filessystem in a bad state! Test thoroughly 
         using mv on (filled and empty) directories and files onto 
         inexistant and already existing directories and files.

   (16)  Design, implement and test any function that your instructor
         might have left out from this list. There are 13 functions 
         __myfs_XXX_implem you have to write.

   (17)  Go over all functions again, testing them one-by-one, trying
         to exercise all special conditions (error conditions): set
         breakpoints in gdb and use a sequence of bash commands inside
         your mounted filesystem to trigger these special cases. Be
         sure to cover all funny cases that arise when the filesystem
         is full but files are supposed to get written to or truncated
         to longer length. There must not be any segfault; the user
         space program using your filesystem just has to report an
         error. Also be sure to unmount and remount your filesystem,
         in order to be sure that it contents do not change by
         unmounting and remounting. Try to mount two of your
         filesystems at different places and copy and move (rename!)
         (heavy) files (your favorite movie or song, an image of a cat
         etc.) from one mount-point to the other. None of the two FUSE
         processes must provoke errors. Find ways to test the case
         when files have holes as the process that wrote them seeked
         beyond the end of the file several times. Your filesystem must
         support these operations at least by making the holes explicit 
         zeros (use dd to test this aspect).

   (18)  Run some heavy testing: copy your favorite movie into your
         filesystem and try to watch it out of the filesystem.