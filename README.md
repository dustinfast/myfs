# Team3a - Homework 4: 

A FUSE (Filesystem in user space) based file system.

## Contributors

Dustin Fast, Joel Keller, Brooks Woods

![Contributors](https://github.com/dustinfast/myfs/raw/master/img/contributors.png "Contributors")

## Usage

Compile with:
``` sh
gcc myfs.c -Wall implementation.c `pkg-config fuse --cflags --libs` -o myfs
```

To mount:  

``` sh
      # Mount (w/no backup file)
      ./myfs PATH -f
    
      # Mount (w/backup & restore from file)
      ./myfs --backupfile=test.myfs PATH -f

      # To unmount (from a seperate terminal)
      fusermount -u MOUNT_MOUT
```

Where PATH is an empty directory on your existing file system.)
    
After mounting, open a new terminal and navigate to the filesystem's root at PATH.

## Implementation


### Design

![Design](https://github.com/dustinfast/myfs/raw/master/img/fs_design.jpg "Design")
Design by: Dustin Fast
    


### Directory Lookup Table Format
Directory contents are denoted by each directory's inode, according to the format `label:offset\n`. For example:  
`dir1:offset\ndir2:offset\nfile1:offset`

Denotes a directory having the contents

```
./
|  file1
|
+--- dir1
|
+--- dir2
```
        
### Design Decisions
The design was chosen with the following requirements in mind:

1. ...

Additionally, the following assumptions were made:
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


Testing was performed on Debian 4.9 and Ubunutu 18 to verify the following -


| Test | Result | TTI w/our Implementation | TTI w/ stdlib Implementation |
| --------------------------------- | ---------- | --------- | -------- |

Testing performed by: Dustin Fast


Report by: Dustin Fast

