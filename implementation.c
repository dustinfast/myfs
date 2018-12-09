/*

  MyFS: a tiny file-system written for educational purposes

  MyFS is Copyright 2018 University of Alaska Anchorage, College of Engineering.

  Author: 
    Dustin Fast

  Outline/Stubs
    Christoph Lauter
                
  Contributors:
    Joel Keller
    Brooks Woods

  Based on:
    FUSE: Filesystem in Userspace
    Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file LICENSE.

  Usage:
    After mounting, open a new terminal and navigate to the fs at ~/fuse-mnt.

    Compile with:
      gcc myfs.c implementation.c `pkg-config fuse --cflags --libs` -o myfs -g
    
    Mount (w/out backup to file):
      ./myfs ~/fuse-mnt/ -f
    Mount (w/backup to file):
      ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f
    Mount (inside gdb):
      gdb --args ./myfs ~/fuse-mnt/ -f OR
      gdb --args ./myfs --backupfile=test.myfs ~/fuse-mnt/ -f
    Unmount:
      fusermount -u ~/fuse-mnt

    For more information, see README.md.
*/

#include <stddef.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

#include "implementation.h"  // Custom defs and helpers


/* Begin Filesystem Helpers ----------------------------------------------- */


static Inode* resolve_path(FSHandle *fs, const char *path);  // Prototype

// Returns a handle to a myfs filesystem on success.
// On fail, sets errnoptr to EFAULT and returns NULL.
static FSHandle *fs_handle(void *fsptr, size_t fssize, int *errnoptr) {
    FSHandle *fs = fs_init(fsptr, fssize);
    if (!fs && errnoptr) *errnoptr = EFAULT;
    return fs;
}

// A debug function to simulate a resolve_path() call. 
// Returns some hardcoded test inode (ex: the root dir).
// On fail, sets errnoptr to ENOENT and returns NULL.
// Assumes: path is absolute.
static Inode *fs_pathresolve(FSHandle *fs, const char *path, int *errnoptr) {
    if (strncmp(path, FS_PATH_SEP, 1) != 0) {
        *errnoptr = EINVAL;
        return NULL;                // No root dir symbol in path
    }

    Inode *inode = resolve_path(fs, path);
    if (!inode && errnoptr) *errnoptr = ENOENT;
    return inode;
}


/* End Filesystem Helpers ------------------------------------------------- */
/* Begin Inode helpers ---------------------------------------------------- */


// Populates buf with a string representing the given inode's data.
// Returns: The size of the data at buf.
// NOTE: buf should be pre-sized with malloc(inode->file_size_b)
static size_t inode_data_get(FSHandle *fs, Inode *inode, const char *buf) {
    inode_lasttimes_set(inode, 0);
    return memblock_data_get(fs, inode_firstmemblock(fs, inode), buf);   
}

// Disassociates any data from inode, formats any previously used memblocks,
// and assign the inode a new free first memblock.
static void inode_data_remove(FSHandle *fs, Inode *inode) {
    MemHead *memblock = inode_firstmemblock(fs, inode);
    MemHead *block_next;     // ptr to memblock->offset_nextblk
    void *block_end;         // End of memblock's data field

     // Format each memblock of the inode's data
    do {
        block_next = (MemHead*)ptr_from_offset(fs, memblock->offset_nextblk);
        block_end = (void*)memblock + MEMBLOCK_SZ_B;         // End of memblock
        memset(memblock, 0, (block_end - (void*)memblock));  // Format memblock
        memblock = (MemHead*)block_next;                     // Advance to next
        
    } while (block_next != (MemHead*)fs);  // i.e. memblock->offset_nextblk == 0

    // Update the inode to reflect the disassociation and replace memblock
    inode->file_size_b = 0;
    // *(int*)(&inode->file_size_b) = 0;   // TODO: Test
    inode->offset_firstblk = (size_t*)offset_from_ptr(fs, memblock_nextfree(fs));
    inode_lasttimes_set(inode, 1);
}

// Sets data field and updates size fields for the file or dir denoted by
// inode including handling of the  linked list of memory blocks for the data.
// Assumes: Filesystem has enough free memblocks to accomodate data.
// Assumes: inode has its offset_firstblk set.
static void inode_data_set(FSHandle *fs, Inode *inode, char *data, size_t sz) {
    // Format the inode, if needed
    if (inode->file_size_b)
        inode_data_remove(fs, inode);

    MemHead *memblock = inode_firstmemblock(fs, inode);

    // Use a single block if sz will fit in one
    if (sz <= DATAFIELD_SZ_B) {
        void *data_field = memblock + ST_SZ_MEMHEAD;
        memcpy(data_field, data, sz);
        *(int*)(&memblock->not_free) = 1;
        memblock->data_size_b = (size_t*) sz;
        memblock->offset_nextblk = 0;
    }

    // Else use multiple blocks, if available
    else {
        // Determine num blocks needed
        size_t num_bytes = sz;
        
        // Debug
        // size_t blocks_needed = num_bytes / DATAFIELD_SZ_B;
        // printf("Need %lu blocks for these %lu bytes\n ", blocks_needed, num_bytes);

        // Populate the memory blocks with the data
        char *data_idx = data;
        MemHead *prev_block = NULL;
        size_t write_bytes = 0;
        
        while (num_bytes) {
            // Determine num bytes to write this iteration
            if (num_bytes > DATAFIELD_SZ_B)
                write_bytes = DATAFIELD_SZ_B;  // More blocks will be needed
            else
                write_bytes = num_bytes;       // Last block needed

            // Denote ptr to write addr
            char *ptr_writeto = memblock_datafield(fs, memblock);

            // Write the bytes to the data field
            memcpy(ptr_writeto, data_idx, write_bytes);
            *(int*)(&memblock->not_free) = 1;
            *(size_t*)(&memblock->data_size_b) = write_bytes;

            // Update next block offsets as needed
            if (prev_block) 
                prev_block->offset_nextblk = (size_t*)offset_from_ptr(fs, (void*)memblock);
            prev_block = memblock;
            memblock->offset_nextblk = 0;
        
            // Set up the next iteration
            data_idx += write_bytes;
            num_bytes = num_bytes - write_bytes; // Adjust num bytes to write
            memblock = memblock_nextfree(fs); // Adavance to next free memblock
        }
    }

    inode_lasttimes_set(inode, 1);
    inode->file_size_b = (size_t*) sz;
}

// Appends the given data to the given Inode's current data. For appending
// a file/dir "label:offset\n" line to the directory, for example.
// No validation is performed on append_data. Assumes: append_data is a string.
// Returns 1 on success, else 0.
static int inode_data_append(FSHandle *fs, Inode *inode, char *append_data) {
    // Get the parent dir's curr data (i.e. a list of files/dirs)
    size_t data_sz = 0;
    size_t append_sz = str_len(append_data);
    char *data = malloc(*(int*)(&inode->file_size_b) + append_sz + 1); // TODO: +1?
    data_sz = inode_data_get(fs, inode, data);
    size_t total_sz = data_sz + append_sz;

    memcpy(data + data_sz, append_data, append_sz);  // Build concat of data
    inode_data_set(fs, inode, data, total_sz);      // Overwrite existing data
    free(data);

    // printf("CALLING SET FOR %lu bytes w:\n", total_sz);
    // write(fileno(stdout), data, total_sz);

    return 1; // Success
}


/* End Inode helpers ------------------------------------------------------ */
/* Begin Directory helpers ------------------------------------------------ */


// Returns the inode for the given item (a sub-directory or file) having the
// parent directory given by inode (Or NULL if item could not be found).
static Inode* dir_subitem_get(FSHandle *fs, Inode *inode, char *itemlabel) {
    if (!inode) return NULL;  // Ensure valid inode ptr

    // Get parent dir's data
    char *curr_data = malloc(*(int*)(&inode->file_size_b));
    inode_data_get(fs, inode, curr_data);

    // Get ptr to the items line in the parent dir's file/dir data.
    char *subdir_ptr = strstr(curr_data, itemlabel);

    // If subdir does not exist, return NULL
    if(subdir_ptr == NULL) {
        free(curr_data);
        return NULL;
    }

    // Else, extract the subdir's inode offset
    char *offset_ptr = strstr(subdir_ptr, FS_DIRDATA_SEP);
    char *offsetend_ptr = strstr(subdir_ptr, FS_DIRDATA_END);

    if (!offset_ptr || !offsetend_ptr) {
        printf("ERROR: Parse fail - Dir data may be corrupt -\n");
        printf("parent = %s child = %s\n", inode->name, itemlabel);
        free(curr_data);
        return NULL;
    }

    size_t offset;
    size_t offset_sz = offsetend_ptr - offset_ptr;
    char *offset_str = malloc(offset_sz);
    memcpy(offset_str, offset_ptr + 1, offset_sz - 1);  // +/- 1 to exclude sep
    sscanf(offset_str, "%zu", &offset); // Convert offset from str to size_t

    Inode *subdir_inode = (Inode*)ptr_from_offset(fs, (size_t*)offset);

    // debug
    // printf("Subdir Offset: %lu\n", offset);
    // printf("Returning subdir w/name: %s\n", subdir_inode->name);

    // Cleanup
    free(curr_data);
    free(offset_str);

    return subdir_inode;
}

// Creates a new/empty sub-directory under the parent dir specified by inode.
// Returns: A ptr to the newly created dir's inode on success, else NULL.
static Inode* dir_new(FSHandle *fs, Inode *inode, char *dirname) {
    // Validate...
    if (!inode_isdir(inode)) {
        // printf("ERROR: %s is not a directory\n", dirname);
        return NULL; 
    } 
    if (!inode_name_isvalid(dirname)) {
            // printf("ERROR: %s is not a valid directory name\n", dirname);
            return NULL;
    }
    if(dir_subitem_get(fs, inode, dirname) != NULL) {
        // printf("ERROR: %s already exists\n", dirname);
        return NULL;
    }

    // Begin creating the new directory...
    Inode *newdir_inode = inode_nextfree(fs);
    MemHead *newdir_memblock = memblock_nextfree(fs);
    newdir_inode->offset_firstblk = (size_t*)offset_from_ptr(fs, 
        (void*)newdir_memblock);

    if (newdir_inode == NULL || newdir_memblock == NULL) {
        printf("ERROR: Failed to get resources adding %s\n", dirname);
        return NULL;
    }

    // Get the new inode's offset
    char offset_str[1000];      // TODO: sz should be based on fs->num_inodes
    size_t offset = offset_from_ptr(fs, newdir_inode);        // offset
    snprintf(offset_str, sizeof(offset_str), "%lu", offset);  // offset to str

    // Build new directory's lookup line: "dirname:offset\n"
    size_t data_sz = 0;
    data_sz = str_len(dirname) + str_len(offset_str) + 2; // +2 for : and \n
    char *data = malloc(data_sz);

    strcpy(data, dirname);
    strcat(data, FS_DIRDATA_SEP);
    strcat(data, offset_str);
    strcat(data, FS_DIRDATA_END);

    //debug
    // printf("\n- Adding new dir: %s To: %s\n", dirname, inode->name);
    // printf("  New lookup line to write: %s\n", data);

    // Append the lookup line to the parent dir's existing lookup data
    inode_data_append(fs, inode, data);
    
    // Update parent dir properties
    *(int*)(&inode->subdirs) = *(int*)(&inode->subdirs) + 1;
    
    // Set new dir's properties
    inode_name_set(newdir_inode, dirname);
    *(int*)(&newdir_inode->is_dir) = 1;
    inode_data_set(fs, newdir_inode, "", 0); 

    return newdir_inode;
}

// Removes the directory denoted by the given inode from the file system.
// Returns 1 on success, else 0.
static int dir_remove(FSHandle *fs, const char *path) {
    Inode *parent;
    Inode *dir;

    // Split the given path into seperate path and filename elements
    char *par_path, *dirname;
    char *start, *token, *next;

    par_path = malloc(1);            // Init abs path array
    *par_path = '\0';
    
    start = next = strdup(path);    // Duplicate path so we can manipulate it
    next++;                         // Skip initial seperator

    while ((token = strsep(&next, FS_PATH_SEP))) {
        if (!next) {
            dirname = token;
        } else {
            par_path = realloc(par_path, str_len(par_path) + str_len(token) + 1);
            strcat(par_path, FS_PATH_SEP);
            strcat(par_path, token);
        }
    }

    if (*par_path == '\0')
        strcat(par_path, FS_PATH_SEP);

    // Get the inodes for the parent and dir
    parent = resolve_path(fs, par_path);
    dir = resolve_path(fs, dirname);

    // Ensure valid parent/dir before continuing
    if (parent && dir) {
    // Denote dir's offset, in str form
        char offset_str[1000];   // TODO: sz should be based on fs->num_inodes
        size_t dir_offset = offset_from_ptr(fs, dir);
        snprintf(offset_str, sizeof(offset_str), "%lu", dir_offset);

        // Remove dir's lookup line (ex: "dirname:offset\n") from parent
        char *dir_name = strdup(dir->name);
        size_t data_sz = 0;
        data_sz = str_len(dir_name) + str_len(offset_str) + 2; // +2 for : and \n
        char *rmline = malloc(data_sz);

        strcpy(rmline, dir_name);
        strcat(rmline, FS_DIRDATA_SEP);
        strcat(rmline, offset_str);
        strcat(rmline, FS_DIRDATA_END);

        // Read file data
        char* par_data = malloc(*(int*)(&parent->file_size_b));
        size_t par_data_sz = inode_data_get(fs, parent, par_data);

        // Denote the start/end of the dir's lookup line
        size_t line_sz = str_len(rmline);
        char *line_start = strstr(par_data, rmline);
        char *offsetend_ptr = line_start + line_sz;

        // Build new parent data without the dir's lookup line
        char *new_data = malloc(par_data_sz - line_sz);  // freeme?
        size_t sz1 =  line_start - par_data;
        size_t sz2 = par_data_sz - sz1 - line_sz;
        memcpy(new_data, par_data, sz1);
        memcpy(new_data + sz1, offsetend_ptr, sz2);
        
        // debug
        // printf("Removing dir -\npath: %s\ndirname: %s\n", path, dirname);
        // printf("Parent data (len=%lu): %s \n", par_data_sz, par_data);
        // printf("Remove line (len=%lu): %s", line_sz, rmline);
        // printf("sz1 = %lu, sz2 = %lu\n\n", sz1, sz2);

        // Update the parent to reflect removal of dir
        inode_data_set(fs, parent, new_data, sz1 + sz2);
        *(int*)(&parent->subdirs) = *(int*)(&parent->subdirs) - 1;

        // Format the dir's inode
        inode_data_remove(fs, dir); 
        *(int*)(&dir->is_dir) = 0;
        *(int*)(&dir->subdirs) = 0;

        free(par_path);
        free(start);
        free(rmline);
        free(dir_name);
        // if (freeme != NULL)
        //     free(freeme);
    
        return 1; // Success

    } else
        return 0; // Fail (bad path)
}


/* End Directory helpers ------------------------------------------------- */
/* Begin File helpers ---------------------------------------------------- */

// Creates a new file in the fs having the given properties.
// Note: path is parent dir path, fname is the file name. Ex: '/' and 'file1'.
// Returns: A ptr to the newly created file's I-node (or NULL on fail).
static Inode *file_new(FSHandle *fs, char *path, char *fname, char *data,
                       size_t data_sz) {
    Inode *parent = resolve_path(fs, path);

    if (!parent) {
        // printf("ERROR: invalid path\n");
        return NULL;
    }
    
    if(dir_subitem_get(fs, parent, fname) != NULL) {
        // printf("ERROR: File already exists\n");
        return NULL;
    }

    if (memblocks_numfree(fs) <  data_sz / DATAFIELD_SZ_B) {
        printf("ERROR: Insufficient free memblocks for new file %s\n", fname);
        return NULL;
    }

    Inode *inode = inode_nextfree(fs);
    if (!inode) {
        printf("ERROR: Failed getting free inode for new file %s\n", fname);
        return NULL;
    }

    MemHead *memblock = memblock_nextfree(fs);
    if (!memblock) {
        printf("ERROR: Failed getting free memblock for new file %s\n", fname);
        return NULL;
    }

    if (!inode_name_set(inode, fname)) {
        // printf("ERROR: Invalid file name\n");
        return NULL;
    }
    
    // Associate first memblock with the inode (by it's offset)
    size_t offset_firstblk = offset_from_ptr(fs, (void*)memblock);
    inode->offset_firstblk = (size_t*)offset_firstblk;
    inode_data_set(fs, inode, data, data_sz);
    
    // Get the new file's inode offset
    char offset_str[1000];      // TODO: sz should be based on fs->num_inodes
    size_t offset = offset_from_ptr(fs, inode);               // offset
    snprintf(offset_str, sizeof(offset_str), "%zu", offset);  // offset to str

    // Build new file's lookup line: "filename:offset\n"
    size_t fileline_sz = 0;
    fileline_sz = str_len(fname) + str_len(offset_str) + 2; // +2 for : and \n

    char *fileline_data = malloc(fileline_sz);
    strcpy(fileline_data, fname);
    strcat(fileline_data, FS_DIRDATA_SEP);
    strcat(fileline_data, offset_str);
    strcat(fileline_data, FS_DIRDATA_END);
    
    //debug
    // printf("\n- Adding new file: %s To: %s\n", fname, parent->name);
    // printf("  New lookup line to write: %s (%lu bytes)\n", fileline_data, fileline_sz);

    // Append the lookup line to the parent dir's existing lookup data
    inode_data_append(fs, parent, fileline_data);
    free(fileline_data);

    return inode;
}


// Resolves the given file or directory path and returns its associated inode.
// Author: Joel Keller
static Inode* resolve_path(FSHandle *fs, const char *path) {
    Inode* root_dir = fs_rootnode_get(fs);

    // If path is root directory 
    if (strcmp(path, root_dir->name) == 0)
        return root_dir;

    Inode* curr_dir = root_dir;
    int curr_ind = 1;
    int path_ind = 0;
    int path_len = strlen(path);
    char curr_path_part[path_len];
    while (curr_ind < path_len) {
        path_ind = 0;
        while (1) {
            curr_path_part[path_ind] = path[curr_ind];
            curr_ind = curr_ind + 1;
            path_ind = path_ind + 1;
            if (path[curr_ind] == '/' || path[curr_ind] == '\0') {
                curr_ind = curr_ind + 1;
                break;
            }
        }
        curr_path_part[path_ind] = '\0';
        curr_dir = dir_subitem_get(fs, curr_dir, curr_path_part);
    }
    return curr_dir;
}

/* End File helpers ------------------------------------------------------- */
/* Begin emulation functins ----------------------------------------------- */


/* Implements the "stat" system call on the filesystem 
   Accepts:
      fsptr       : ptr to the fs
      fssize      : size of fs pointed to by fsptr
      errnoptr    : Error container
      uid         : User ID of file/dir owner
      gid         : Group ID of file/dir owner
      path        : Path of the file/dir in question
      stbuf       : Results container

   Returns:
      -1  (w/error in *errnoptr) iff path not a valid file or directory. 
      Else returns 0 with file/dir info copied to stbuf as -
            nlink_t       Count of subdirectories (w/ . & ..), or just 1 if file)
            uid_t         Owners's user ID (from args)
            gid_t         Owner's group ID (from args)
            off_t         Real file size, in bytes (for files only)
            st_atim       Last access time
            st_mtim       Last modified time
            mode_t        File type/mode as S_IFDIR | 0755 for directories,
                                            S_IFREG | 0755 for files
*/
int __myfs_getattr_implem(void *fsptr, size_t fssize, int *errnoptr,
                          uid_t uid, gid_t gid,
                          const char *path, struct stat *stbuf) {                          
    FSHandle *fs = NULL;       // Handle to the file system
    Inode *inode = NULL;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

    //Reset the memory of the results container
    memset(stbuf, 0, sizeof(struct stat));

    //Populate stdbuf with the atrributes of the inode
    stbuf->st_uid = uid;
    stbuf->st_gid = gid;
    stbuf->st_atim = *inode->last_acc; 
    stbuf->st_mtim = *inode->last_mod;    
    
    if (inode->is_dir) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = *(int*)(&inode->subdirs) + 2;  // "+ 2" for . and .. 
    } else {
        stbuf->st_mode = S_IFREG | 0755;
        stbuf->st_nlink = 1;
        stbuf->st_size = *(int*)(&inode->file_size_b);
    } 

    return 0;  // Success  
}

/* Implements an emulation of the readdir system call on the filesystem 
   of size fssize pointed to by fsptr. 

   If path can be followed and describes a directory that exists and
   is accessable, the names of the subdirectories and files 
   contained in that directory are output into *namesptr. The . and ..
   directories must not be included in that listing.

   If it needs to output file and subdirectory names, the function
   starts by allocating (with calloc) an array of pointers to
   characters of the right size (n entries for n names). Sets
   *namesptr to that pointer. It then goes over all entries
   in that array and allocates, for each of them an array of
   characters of the right size (to hold the i-th name, together 
   with the appropriate '\0' terminator). It puts the pointer
   into that i-th array entry and fills the allocated array
   of characters with the appropriate name. The calling function
   will call free on each of the entries of *namesptr and 
   on *namesptr.

   The function returns the number of names that have been 
   put into namesptr. 

   If no name needs to be reported because the directory does
   not contain any file or subdirectory besides . and .., 0 is 
   returned and no allocation takes place.

   On failure, -1 is returned and the *errnoptr is set to 
   the appropriate error code. 

   The error codes are documented in man 2 readdir.

   In the case memory allocation with malloc/calloc fails, failure is
   indicated by returning -1 and setting *errnoptr to EINVAL.

*/
int __myfs_readdir_implem(void *fsptr, size_t fssize, int *errnoptr,
                          const char *path, char ***namesptr) {
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

    // Ensure path denotes a dir
    if (!inode->is_dir) {
        *errnoptr = EINVAL;
        return -1;
    }

    // Get the directory's lookup table and add an extra end char
    char *data = malloc(*(int*)(&inode->file_size_b) + 1);
    size_t data_sz = inode_data_get(fs, inode, data);
    memcpy(data + data_sz + 1, FS_DIRDATA_END, 1);

    // Build the names array from the lookup table data
    char *token, *name, *next;
    size_t names_count = 0;
    size_t names_len = 0;

    char *names = malloc(0);
    next = name = data;
    while ((token = strsep(&next, FS_DIRDATA_END))) {
        if (!next || !inode_name_charvalid(*token)) // Ignore last sep and null
            break;

        if (*token <= 64) break;
        name = token;                               // Extract file/dir name
        name = strsep(&name, FS_DIRDATA_SEP);
        int nlen = strlen(name) + 1;                // +1 for null term
        names_len += nlen;              
        names = realloc(names, names_len);          // Make room for new item    

        memcpy(names + names_len - nlen, name, nlen - 1);
        memset(names + names_len - 1, '\0', 1);
        names_count++;
        
        // printf("name: %s\n", name);              // debug
        // printf("nameslen: %lu\n", names_len);
        // printf("set 1    : %lu\n", names_len - nlen);
        // printf("set 2    : %lu\n\n", names_len - 1);
        // write(fileno(stdout), names, names_len); printf("\n");
    }


    if (names_count)
        namesptr = names;

    free(data);

    return names_count;
}

/* Implements an emulation of the mknod system call for regular files
   on the filesystem of size fssize pointed to by fsptr.

   This function is called only for the creation of regular files.

   If a file gets created, it is of size zero and has default
   ownership and mode bits.

   The call creates the file indicated by path.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 mknod.

*/
int __myfs_mknod_implem(void *fsptr, size_t fssize, int *errnoptr,
                        const char *path) {
    FSHandle *fs;       // Handle to the file system

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Check for root dir symbol in path (assumes path is absolute)
    if (strncmp(path, FS_PATH_SEP, 1) != 0) {
        *errnoptr = EINVAL;
        return -1;
    }

    // Split the given path into seperate path and filename elements
    char *abspath, *fname;
    char *start, *token, *next;

    abspath = malloc(1);            // Init abs path array
    *abspath = '\0';
    
    start = next = strdup(path);    // Duplicate path so we can manipulate it
    next++;                         // Skip initial seperator

    while ((token = strsep(&next, FS_PATH_SEP))) {
        if (!next) {
            fname = token;
        } else {
            abspath = realloc(abspath, str_len(abspath) + str_len(token) + 1);
            strcat(abspath, FS_PATH_SEP);
            strcat(abspath, token);
        }
    }

    if (*abspath == '\0')
        strcat(abspath, FS_PATH_SEP);  // TODO: Test strncpy here instead

    // Create the file
    // printf("Creating File -\nabspath: %s\nfname: %s\n", abspath, fname); // Debug
    Inode *newfile = file_new(fs, abspath, fname, "", 0);
    
    // Cleanup
    free(abspath);
    free(start);

    if (!newfile) {
        *errnoptr = EINVAL;
        return -1;  // Fail - bad fname, or file already exists
    }
    return 0;  // Success
}

/* Implements an emulation of the unlink system call for regular files
   on the filesystem of size fssize pointed to by fsptr.

   This function is called only for the deletion of regular files.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 unlink.

*/
int __myfs_unlink_implem(void *fsptr, size_t fssize, int *errnoptr,
                        const char *path) {
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

    /* STUB */
    
    return -1;
}

/* Implements an emulation of the rmdir system call on the filesystem 
   of size fssize pointed to by fsptr. 

   The call deletes the directory indicated by path.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The function call must fail when the directory indicated by path is
   not empty (if there are files or subdirectories other than . and ..).

   The error codes are documented in man 2 rmdir.

*/
int __myfs_rmdir_implem(void *fsptr, size_t fssize, int *errnoptr,
                        const char *path) {
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

    // Ensure dir empty
    if (inode->file_size_b)  {
        *errnoptr = ENOTEMPTY;
        return -1;
    }

    dir_remove(fs, path);

    return 0; // success
}

/* Implements an emulation of the mkdir system call on the filesystem 
   of size fssize pointed to by fsptr. 

   The call creates the directory indicated by path.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 mkdir.

*/
int __myfs_mkdir_implem(void *fsptr, size_t fssize, int *errnoptr,
                        const char *path) {
    FSHandle *fs;       // Handle to the file system

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1;

    // Check for root dir symbol in path (assumes path is absolute)
    if (strncmp(path, FS_PATH_SEP, 1) != 0) {
        *errnoptr = EINVAL;
        return -1;
    }

    // Split the given path into seperate path and filename elements
    char *par_path, *name;
    char *start, *token, *next;
    
    start = next = strdup(path);    // Duplicate path so we can manipulate it
    next++;                         // Skip initial seperator

    par_path = malloc(1);           // Parent path buffer
    *par_path = '\0';
    while ((token = strsep(&next, FS_PATH_SEP))) {
        if (!next) {
            name = token;
        } else {
            par_path = realloc(par_path, str_len(par_path) + str_len(token) + 1);
            strcat(par_path, FS_PATH_SEP);
            strcat(par_path, token);
        }
    }

    if (*par_path == '\0')
        strcat(par_path, FS_PATH_SEP);  // TODO: Test strncpy here instead

    // Create the fdir
    // printf("Creating dir -\npar_path: %s\nname: %s\n", par_path, name); // Debug
    Inode *parent = fs_pathresolve(fs, par_path, errnoptr);
    Inode *newdir = dir_new(fs, parent, name);
    
    // Cleanup
    free(par_path);
    free(start);

    if (!newdir)
        return -1;  // Fail - bad path, or already exists
    return 0;       // Success
    
}

/* Implements an emulation of the rename system call on the filesystem 
   of size fssize pointed to by fsptr. 

   The call moves the file or directory indicated by from to to.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

    Behavior (from man 2 readme):
    If  newpath  already  exists,  it  will   be   atomically
    replaced,  so  that  there  is  no point at which another
    process attempting to access newpath will find  it  miss‐
    ing.   However,  there will probably be a window in which
    both oldpath and newpath refer to the file being renamed.

    If oldpath and newpath are existing hard links  referring
    to the same file, then rename() does nothing, and returns
    a success status.

    If newpath exists but the operation fails for  some  rea‐
    son,  rename() guarantees to leave an instance of newpath
    in place.

    oldpath can specify a directory.  In this  case,  newpath
    must either not exist, or it must specify an empty direc‐
    tory.

    If oldpath  refers  to  a  symbolic  link,  the  link  is
    renamed;  if  newpath refers to a symbolic link, the link
    will be overwritten.

   The error codes are documented in man 2 rename.

*/
int __myfs_rename_implem(void *fsptr, size_t fssize, int *errnoptr,
                         const char *from, const char *to) {
    if (strcmp(from, to) == 0) return 0;  // No work required
    
    FSHandle *fs;           // Handle to the file system

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get the indexes of the file/dir seperators and path sizes
    size_t from_len, to_len;
    size_t from_idx = path_name_offset(from, &from_len);
    size_t to_idx = path_name_offset(to, &to_len);

    char *from_path = strndup(from, (from_idx > 1 ) ? from_idx-1 : from_idx);
    char *to_path = strndup(to, to_idx);
    char *to_name = strndup(to + to_idx, to_len - to_idx);

    // Debug
    printf("\nRenaming: %s\n", from);
    printf("To: %s\n", to);
    printf("From parent path: %s (%lu)\n", from_path, from_len);
    printf("To parent path: %s (%lu)\n", to_path, from_idx);
    printf("From name: %s (%lu)\n", from_path, from_len);
    printf("To name: %s\n", to_name);

    Inode *from_parent = fs_pathresolve(fs, from_path, errnoptr);
    Inode *to_parent = fs_pathresolve(fs, to_path, errnoptr);
    free(from_path);

    Inode *from_child = fs_pathresolve(fs, from, errnoptr);
    Inode *to_child = fs_pathresolve(fs, to, errnoptr);
    
    // Ensure all the boys are in the band
    if (!from_parent || !from_child || !to_parent) {
        *errnoptr = EINVAL;
        free(to_name);
        free(to_path);
        return -1;
    }

    // Begin the move...
    char *data = malloc(*(int*)(&from_child->file_size_b));
    size_t sz; 

    // If 'from' is a directory, 'to' must either not exist or be an empty dir
    if(from_child->is_dir) {
        // If the destination doesn't exist, create it and move the data
        if(!to_child) {
            Inode *dest = dir_new(fs, to_parent, to_name);  // Create dest
            sz = inode_data_get(fs, from_child, data);      // Get old data
            inode_data_set(fs, dest, data, sz);             // Copy to dest
            dir_remove(fs, from);                           // Remove old dir
        } 
        
        // If dest does exist and is empty, overwrite it with 'to'
        else if (to_child->is_dir && !to_child->file_size_b) {
            dir_remove(fs, to);                             // Remove dest            
            Inode *dest = dir_new(fs, to_parent, to_name);  // Recreate dest
            sz = inode_data_get(fs, from_child, data);      // Get old data
            inode_data_set(fs, dest, data, sz);             // Copy to dest
            dir_remove(fs, from);                           // Remove old dir
        }
        
        // Else, error
        else {
            *errnoptr = EINVAL;
            free(to_name);
            free(to_path);
            return -1;
        }
    }

    // Else, 'from' is a regular file
    else {
        sz = inode_data_get(fs, from_child, data);   // Get old data
        
        // If the destination exists, atomically overwrite it
        if(to_child)
            inode_data_set(fs, to_child, data, sz); // TODO: atomically

        // Else, create it
        else
            file_new(fs, to_path, to_name, data, sz);

        // TODO: Remove old file
    }

    free(data);    
    free(to_name);
    free(to_path);

    return 0;
}

/* Implements an emulation of the truncate system call on the filesystem 
   of size fssize pointed to by fsptr. 

   The call changes the size of the file indicated by path to offset
   bytes.

   When the file becomes smaller due to the call, the extending bytes are
   removed. When it becomes larger, zeros are appended.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 truncate.

*/
int __myfs_truncate_implem(void *fsptr, size_t fssize, int *errnoptr,
                           const char *path, off_t offset) {
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

    // Read file data
    char* orig_data = malloc(*(int*)(&inode->file_size_b));
    size_t data_size = inode_data_get(fs, inode, orig_data);

    // If request makes file larger
    if (offset > data_size) {
        size_t diff = offset- data_size;
        // printf("Expanding (offset=%lu, data_sz=%lu, diff=%lu)...\n", offset, data_size, diff);  // debug
        char *diff_arr = malloc(diff);
        memset(diff_arr, 0, diff);
        inode_data_append(fs, inode, diff_arr);     // Append zeroes to file
    }
    // Else, if request makes file smaller
    else if (offset < data_size) {
        // printf("Shrinking (offset=%lu, data_sz=%lu)...\n", offset, data_size);  // debug
        inode_data_set(fs, inode, orig_data, offset);
    }
    // Otherwise, file size and contents are unchanged

    return 0;  // Success
}

/* -- __myfs_open_implem() -- */
/* Implements an emulation of the open system call on the filesystem 
   of size fssize pointed to by fsptr, without actually performing the opening
   of the file (no file descriptor is returned).

   The call just checks if the file (or directory) indicated by path
   can be accessed, i.e. if the path can be followed to an existing
   object for which the access rights are granted.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The two only interesting error codes are 

   * EFAULT: the filesystem is in a bad state, we can't do anything

   * ENOENT: the file that we are supposed to open doesn't exist (or a
             subpath).

   It is possible to restrict ourselves to only these two error
   conditions. It is also possible to implement more detailed error
   condition answers.

   The error codes are documented in man 2 open.

*/
int __myfs_open_implem(void *fsptr, size_t fssize, int *errnoptr,
                       const char *path) {
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;  // Fail

    return 0; // Success
}

/* Implements an emulation of the read system call on the filesystem 
   of size fssize pointed to by fsptr.

   The call copies up to size bytes from the file indicated by 
   path into the buffer, starting to read at offset. See the man page
   for read for the details when offset is beyond the end of the file etc.
   
   On success, the appropriate number of bytes read into the buffer is
   returned. The value zero is returned on an end-of-file condition.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 read.

*/
int __myfs_read_implem(void *fsptr, size_t fssize, int *errnoptr,
                       const char *path, char *buf, size_t size, off_t offset) {
    if (!size) return 0;  // If no bytes to read

    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;
    
    // Read file data
    char* full_buf = malloc(*(int*)(&inode->file_size_b));
    char *cpy_buf = full_buf;
    int cpy_size = 0;
    size_t data_size = inode_data_get(fs, inode, full_buf);

    // If offset is past end of data, zero-bytes readable
    if (offset >= data_size) { 
        free(full_buf);
        return 0;
    }

    // Set data index and num bytes to read based on offset param
    cpy_buf += offset;
    cpy_size = data_size - offset;

    // Copy cpy_size bytes into buf
    memcpy(buf, cpy_buf, cpy_size);
    free(full_buf);

    return cpy_size;  // num bytes written
}

/* Implements an emulation of the write system call on the filesystem 
   of size fssize pointed to by fsptr.

   The call copies up to size bytes to the file indicated by 
   path into the buffer, starting to write at offset. See the man page
   for write for the details when offset is beyond the end of the file etc.
   
   On success, the appropriate number of bytes written into the file is
   returned. The value zero is returned on an end-of-file condition.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 write.

*/
int __myfs_write_implem(void *fsptr, size_t fssize, int *errnoptr,
                        const char *path, const char *buf, size_t size, off_t offset) {
    if (!size) return 0;  // If no bytes to write

    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

    // If offset is 0, we replace all existing data with the given data
    if (!offset) {
        char *dup = strndup(buf, size);
        inode_data_remove(fs, inode);
        inode_data_set(fs, inode, dup, size);
        free(dup);
    }

    // Else, append to existing file data, starting at offset
    else {   
        // Read file's existing data
        char *new_data;
        int new_data_sz;
        char *orig_data = malloc(*(int*)(&inode->file_size_b));
        size_t orig_sz = inode_data_get(fs, inode, orig_data);

        // If offset is beyond end of data, zero-bytes to write
        if (offset >= orig_sz) { 
            free(orig_data);
            return 0;
        }

        // Build 1st half of new_data from existing file data, starting at offset
        new_data = strndup(orig_data, offset);

        // Set 2nd half of new_data: size bytes from the buf param
        new_data_sz = offset + size;
        new_data = realloc(new_data, new_data_sz);
        memcpy(new_data + offset, buf, size);

        // Replace the file's data with the new data
        inode_data_set(fs, inode, new_data, new_data_sz);

        // Cleanup
        free(new_data);
        free(orig_data);
    }

    return size;  // num bytes written
}

/* Implements an emulation of the utimensat system call on the filesystem 
   of size fssize pointed to by fsptr.

   The call changes the access and modification times of the file
   or directory indicated by path to the values in ts.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 utimensat.

*/
int __myfs_utimens_implem(void *fsptr, size_t fssize, int *errnoptr,
                          const char *path, const struct timespec ts[2]) {
    FSHandle *fs;       // Handle to the file system
    Inode *inode;       // Inode for the given path

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    // Get inode for the path (sets erronoptr = ENOENT and returns -1 on fail)
    if ((!(inode = fs_pathresolve(fs, path, errnoptr)))) return -1;

    memcpy(inode->last_acc, &ts[0], sizeof(struct timespec));
    memcpy(inode->last_mod, &ts[1], sizeof(struct timespec));
    
    return 0;
}

/* Implements an emulation of the statfs system call on the filesystem 
   of size fssize pointed to by fsptr.

   The call gets information of the filesystem usage and puts in 
   into stbuf.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 statfs.

   Essentially, only the following fields of struct statvfs need to be
   supported:

   f_bsize   fill with what you call a block (typically 1024 bytes)
   f_blocks  fill with the total number of blocks in the filesystem
   f_bfree   fill with the free number of blocks in the filesystem
   f_bavail  fill with same value as f_bfree
   f_namemax fill with your maximum file/directory name, if your
             filesystem has such a maximum

*/
int __myfs_statfs_implem(void *fsptr, size_t fssize, int *errnoptr,
                         struct statvfs *stbuf) {
    FSHandle *fs;       // Handle to the file system

    // Bind fs handle (sets erronoptr = EFAULT and returns -1 on fail)
    if ((!(fs = fs_handle(fsptr, fssize, errnoptr)))) return -1; 

    size_t blocks_free = memblocks_numfree(fs);
    stbuf->f_bsize = DATAFIELD_SZ_B;
    stbuf->f_blocks = 0;
    stbuf->f_bfree = blocks_free;
    stbuf->f_bavail = blocks_free;
    stbuf->f_namemax = NAME_MAXLEN;

    return 0;
}

/* End emulation functions  ----------------------------------------------- */
/* Begin DEBUG  ----------------------------------------------------------- */


# include "debuglib.h"  // Includes main() and debug output functions
                        // For dev use only - Comment out for prod


/* End DEBUG  ------------------------------------------------------------- */
