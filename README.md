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
    


#### Directory Lookup Table Format
Directory contents and the their associated inode offsets are denoted by each directory inode's memory block(s) according to the format `label:offset\n`. 

For example: `dir1:offset\ndir2:offset\nfile1:offset` denotes a directory having contents

```
./
|
|- file1
|
+--- dir1
|
+--- dir2
```

### Design Decisions
The design was chosen to meet the following requirements:

* Global variables may not be used due to FUSE constraints.
* Pointers to area's in memory must now to be stored into the filesystem in order to facilitate file backup and restore. Instead, offsets must be stored.
* The applicaton must not leak memory or segfault.
* Errors may must not cause the application to exit.

Additionally, the implemenation assumes only absolute paths are ever passed to the 13 funcs "__myfs_..._implem" functions.


## Test Results


Testing was performed on Debian 4.9 and Ubunutu 18 with the following results:

### Positive Testing 

The filesystem was **mounted** via `valgrind --leak-check=full ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f`, after which the following commands were entered in sequence on an empty filesystem and expected to either succeed or fail gracefully -

| Input     |  Output   | Pass/Fail  |
| --------- | --------- | ---------- |
| `cd fuse-mnt`          |  `NONE`         | PASS |
| `mkdir dir1`          |  `NONE`         | PASS |
| `cd dir1`             |  `NONE`         | PASS |     
| `touch file1`         |  `NONE`         | PASS |
| `echo hello > file2` |  `NONE`   |  PASS    |
| `echo world > file2` |  `NONE`   |  PASS    |
| `ls`                  |  `file1 file2`         | PASS |
| `cat file2`           | `hello\nworld`        | PASS|
| `stat file2`          |  `File: test2\nSize: 6 ...` |  PASS    |
| `truncate -s 5 file2` |  `NONE`    |  PASS    |
| `cat file2`           | `hello`        | PASS|
| `mv file1 ../file1`   | `NONE` | PASS |
| `cd ../` | `NONE` | PASS |
| `ls`     |  `dir1 file1`         | PASS |
| `cd ../` | `NONE` | PASS |
| `fusermount - u fuse-mnt` | `NONE` | PASS |

At this point, the file system has been **unmounted** and valgrind displays -

``` sh
==3971== LEAK SUMMARY:
==3971==    definitely lost: 219 bytes in 29 blocks   # FAIL
==3971==    indirectly lost: 32 bytes in 3 blocks     # FAIL
==3971==      possibly lost: 0 bytes in 0 blocks
==3971==    still reachable: 0 bytes in 0 blocks
==3971==         suppressed: 0 bytes in 0 blocks
```

The file system was then **remounted** with `./myfs --backupfile=test.myfs ~/fuse-mnt/ -f` and testing continued as follows -

| Input     |  Output   | Pass/Fail  |
| --------- | --------- | ---------- |
| `cd fuse-mnt`          |  `NONE`         | PASS |
| `ls`                   | `dir2 file1` | PASS |
| `cat dir1/file2`        | `hello` | PASS |
| `rm file3`             | `rm: cannot remove 'file3': No such file or directory` | PASS |
| `rm dir1` |  `rm: cannot remove 'dir1': Is a directory` |  PASS    |
| `rmdir dir1` |  `rmdir: failed to remove 'dir1': Directory not empty` |  PASS |
| `mv dir1/file2 /file2` | `NONE` | PASS |
| `rmdir dir1` |  `NONE` |  PASS |
| `rm file2` | `NONE` | PASS |
| `rm file1` |  `NONE`    |  PASS  |
| `ls` | `NONE` | PASS |
| `wget https://sample-videos.com/video123/mp4/360/big_buck_bunny_360p_30mb.mp4` | `... Saving to 'big_buck_bunny_360p_30mb.mp4'... Success.` | PASS |
| `xdgopen big_buck_bunny_360p_30mb.mp4` | **video begins playing** | PASS |
| `rm big_buck_bunny_360p_30mb.mp4` | **video halts** | FAIL |
| `cd ../` |  `NONE` |  PASS    |
| `fusermount - u fuse-mnt` | `NONE` | PASS |

Testing performed by: Dustin Fast

#### Report by: Dustin Fast

