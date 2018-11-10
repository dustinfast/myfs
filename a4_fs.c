/*
* File system in memory test structure/design.
* Author(s): Joel Keller, Dustin Fast
* Fall, 2018

* Usage: 
    gcc -o fs a4_fs.c
    ./fs

* Notes: 
    See a4_defs.h for definitions
*/

// Includes (as copied from imp.c)
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
#include "a4_defs.h"    // Contains filesystem struct defs and generic helpers

#define FS_SIZE_KB (20) // For debug - Size of the test fs.


/* Begin Filesystem helpers ---------------------------------------------- */

/* Function to add a inode at the end of Filesystem Linked List.
To be used to initialize all the inodes for the disk size */
void push_inode(Inode** head_ref, int number) 
{
    Inode *curr = *head_ref;
    Inode *temp;
	while (curr->next != NULL) {
		temp = curr->next;
        curr = temp;
	}
    // Allocate memory for inode
    // eventually we will be using space from the memory he gives us
    Inode* new_node = (Inode*)malloc(sizeof(Inode)); 

    new_node->inode_number = number;
    new_node->block_start = malloc(BLOCK_SZ_BYTES);
    new_node->is_free = 1;
    new_node->next = NULL; 
	curr->next = new_node;
}

// Find an inode with at least x blocks free
void *find_x_blocks_free(Inode** head_ref, int x)
{
    Inode *curr = *head_ref;
    Inode *first, *temp;
    int i = 0;
    while (curr->next != NULL) {
        if (curr->is_free) {
            if (i == 0) {
                first = curr;
                i++;
            } else if (i < x) {
                i++;
            } else {
                break;
            }
        } else {
            // Not large enough free block
            if (i < x) {
                first = NULL;
                i = 0;
            } else {
                break;
            }
        }
        temp = curr->next;
        curr = temp;
    }
    return first;
}

// Returns the filesystem's current amount of free space in KB as an int.
// TODO: Return size_t instead
int space_free(Inode** head_ref)
{
    Inode *curr = *head_ref;
    Inode *temp;
    int i = 0;
    while (curr != NULL) {
        if (curr->is_free == 1) {
			// printf("Block %d is free.\n", curr->inode_number);  // debug
            i++;
        }
        temp = curr->next;
        curr = temp;
    }
    return (i * BLOCK_SZ_BYTES) / BYTES_IN_KB; // free space in KB
}

// Creates a file and places it at the end of the given directory
void create_file(File** root_dir, Inode** head_ref, int filesize,
                 char* path, char *filename, int is_dir)
{
    if (!is_valid_fname(filename)) {
        printf("Error: Invalid filename specified.");
        return;
    }
    
    // TODO: handle file path for subdirs
    File *curr = *root_dir;
    File *temp;
    Inode *inode, *cur, *tmp;
    int i = 0;
	int blocks = (filesize * BYTES_IN_KB)/BLOCK_SZ_BYTES;
    //find last file in dir
    while (curr->next != NULL) {
        temp = curr->next;
        curr = temp;
    }
    inode = find_x_blocks_free(head_ref, blocks);
	cur = inode;
	
	for (i = 0; i < blocks; i++) {
		cur->is_free = 0;		
		tmp = cur->next;
		cur = tmp;
	}
    
    // Allocate memory for File 
    // eventually we will be using space from the memory he gives us
    File *new = (File*)malloc(sizeof(File)); 
    new->starting_inode = inode;
    new->filesize = filesize;
    strcpy(new->filename, filename);
    new->is_dir = is_dir;
    new->next = NULL;
    curr->next = new;
}

// Clears given directory of all files/inodes, freeing any associated memory.
void empty(File** root_dir, Inode** head_ref)
{
	File *curr = *root_dir;
    File *temp;
    Inode *cur = *head_ref, *tmp;

	// Clear files
	while (curr->next != NULL) {
		temp = curr->next;
		free(curr);
		curr = temp;
	}
	// Clear inodes
	while (cur->next != NULL) {
		tmp = cur->next;
		free(cur->block_start);
		free(cur);
		cur = tmp;
	}
}

/* Initializes a filesystem of the size specified and returns a ptr to it.
Accepts:
    size (size_t)     : Desired size in kbs of the filesystem.
Returns:
    FileSystem*       : Ptr to the resulting FileSystem obj instance.
*/
FileSystem *init_fs(size_t size) {
    FileSystem *fs;                         // Filesystem container
    size_t fs_sz = kb_to_bytes(size);       // Filesystem size, in bytes
    
    if (!is_kb_blockaligned(fs_sz)) {
        printf("ERROR: File system size must be block aligned.");
        return NULL;
    }

    // TODO: Get fsptr, the address for the first memory block in the filesystem
    // TODO: Chunk fsptr space into blocks

    // Build filesystem object "fs"
    fs = (FileSystem*)malloc(sizeof(FileSystem));
    fs->size = fs_sz;

    // Set fs->head
    Inode *head = (Inode*)malloc(sizeof(Inode)); 
	head->inode_number = 0;
    head->block_start = malloc(BLOCK_SZ_BYTES);
    head->next = NULL;
    fs->head = head;
	
	// Build linked list of filesystem inodes starting at 1 (fs->head is blk 0)
	int num_blocks = fs_sz / BLOCK_SZ_BYTES;
	for (int i = 1; i < num_blocks; i++) {
		push_inode(&head, i);
	}

	// Set fs->root (Occupies one block of size BLOCK_SZ_BYTES)
	File *root = (File*)malloc(sizeof(File)); 
    root->starting_inode = head;
	head->is_free = 0;
    root->filesize = BLOCK_SZ_BYTES;
    strcpy(root->filename, FS_ROOTPATH);
    root->is_dir = 1;
    root->next = NULL;

    fs->root = root;

    return fs;
}


/* End Filesystem helpers ------------------------------------------------ */
/* Begin DEBUG helpers --------------------------------------------------- */


// Prints filesystem structure sizes.
void print_struct_szs() {
    printf("Sz of struct 'FileSystem': %lu bytes\n", FS_OBJ_SZ);
    printf("Sz of struct 'File': %lu bytes\n", FILE_OBJ_SZ);
    printf("Sz of struct 'Node': %lu bytes\n", INODE_OBJ_SZ);
}

// print_fs_space(): Prints filesystem stats.
void print_fs_space(FileSystem *fs) {
    printf("%lu KB total space\n", bytes_to_kb(fs->size));
    printf("%d KB free space\n", space_free(&fs->head));
}

// Prints the filesystem's total and currently free amounts of space.
void print_fs_blockstates(FileSystem *fs) {
    Inode *curr = fs->head;
    Inode *temp;
    while (curr != NULL) {
        if (curr->is_free == 1)
			printf("Block %d is free\n", curr->inode_number);
        else
			printf("Block %d is NOT free\n", curr->inode_number);
        temp = curr->next;
        curr = temp;
    }
}

/* End DEBUG helpers ----------------------------------------------------- */
/* Begin Driver program to test above functionality ---------------------- */

int main() 
{
    // Print welcome / struct size info
    printf("Welcome to the file system test space...\n");
    printf("\nFor your information:\n");
    print_struct_szs();
      
    // Init the filesystem object (fs)
	FileSystem *fs = init_fs(FS_SIZE_KB);

    // Print initial fs state
    printf("\nFilesystem intitialized:\n");
	printf("(one %lu KB block used for root dir)\n", bytes_to_kb(BLOCK_SZ_BYTES));
    print_fs_space(fs);
    print_fs_blockstates(fs);
    
	// Create a 8KB test file
	create_file(&fs->root, &fs->head, 8, FS_ROOTPATH, "testfile", 0);

    // Print state after file creation
	printf("\nCreated 8 KB test file in root dir:\n");
	printf("(one %lu KB block used for root dir)\n", bytes_to_kb(BLOCK_SZ_BYTES));
	printf("(two %lu KB blocks used for test file)\n", bytes_to_kb(BLOCK_SZ_BYTES));
    print_fs_space(fs);
    print_fs_blockstates(fs);

    printf("\nExiting...\n");

    // TODO: Write fs contents to backup file

    // TODO: Cleanup/Free mem

    return 0; 
} 
