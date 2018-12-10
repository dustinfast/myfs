# Team3a - Homework 4: 

A FUSE (Filesystem in user space) based file system.

## Contributors

## Usage

Compile with:
``` sh
gcc myfs.c -Wall implementation.c `pkg-config fuse --cflags --libs` -o myfs
```

To mount:
(MOUNT_PATH is an empty directory on your existing file system.)

``` sh
      # Mount (w/no backup file)
      ./myfs MOUNT_PATH -f
    
      # Mount (w/backup & restore from file)
      ./myfs --backupfile=test.myfs MOUNT_PATH -f

      # To unmount (from a seperate terminal)
      fusermount -u MOUNT_MOUT
```
    
After mounting, open a new terminal and navigate to the filesystem's root at MOUNT_PATH.

## Implementation


### File System Design


Author: Dustin Fast
    


### Directory 
    Directory file data field structure:
        Ex: "dir1:offset\ndir2:offset\nfile1:offset"
        Ex: "file1:offset\nfile2:offset"

        
### Design Decisions
The design was chosen with the following requirements in mind


1. ...

Design Decisions:
        Assume a single process accesses the fs at a time?
        To begin writing data before checking fs has enough room?
        Assume only absolute paths passed to 13 funcs?
        Filename and path chars to allow.
        Simple design vs. Better design.
        Inodes to memblocks/memblocks size
        Type of fs layout

These goals led to the following design decisions -
1. **Linked Lists**: ...


## Test Results

Testing was performed on Debian 4.9 and Ubunutu 18. 




