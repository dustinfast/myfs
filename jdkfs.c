/*
* File system in memory test structure/design.
* Author: Joel Keller
* Contributor(s): Dustin Fast
* Date: 11/5/2018

* Usage: 
    gcc -o fs jdkfs.c
    ./fs

*/

// Includes, as copied from imp.c:
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

#define KB_SIZE (1024)
#define BLOCK_SIZE (4096) // 4KB block size
#define FILENAME_LENGTH (256) // 256 char filename

/* The whole filesystem is list of inodes/blocks. 
Files have a inode numbers assigned to them. 
Folders are a list of file objects that have there associated inodes. */
struct Inode 
{ 
    // inode index
    int inode_number;
    // Pointer to start address of block 
    void *block_start;
    // is block free
    int is_free;

    struct Inode *next; 
};

struct File
{
    //first inode if possible we should keep inodes consecutive
    void *starting_inode;
    //file length in KB (multiples of 4 only with 4 KB block size)
    int filesize;
    //filename
    char filename[FILENAME_LENGTH];
    // is directory
    int is_dir;
    //next file in folder or first if dir
    struct File *next;
};

/* Function to add a inode at the end of Filesystem Linked List.
To be used to initialize all the inodes for the disk size */
void push_inode(struct Inode** head_ref, int number) 
{
    struct Inode *curr = *head_ref;
    struct Inode *temp;
	while (curr->next != NULL) {
		temp = curr->next;
        curr = temp;
	}
    // Allocate memory for inode
    // eventually we will be using space from the memory he gives us
    struct Inode* new_node = (struct Inode*)malloc(sizeof(struct Inode)); 

    new_node->inode_number = number;
    new_node->block_start = malloc(BLOCK_SIZE);
    new_node->is_free = 1;
    new_node->next = NULL; 
	curr->next = new_node;
}

// Might be used to find space for new files
void *find_x_blocks_free(struct Inode** head_ref, int x)
{
    struct Inode *curr = *head_ref;
    struct Inode *first, *temp;
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

// get free space in filesystem
int space_free(struct Inode** head_ref)
{
    struct Inode *curr = *head_ref;
    struct Inode *temp;
    int i = 0;
    while (curr != NULL) {
        if (curr->is_free == 1) {
			printf("Block %d is free.\n", curr->inode_number);
            i++;
        }
        temp = curr->next;
        curr = temp;
    }
    return (i * BLOCK_SIZE) / KB_SIZE; // free space in KB
}

//Create file at the end of dir
void create_file(struct File** root_dir, struct Inode** head_ref, int filesize,
                 char* path, char *filename, int is_dir)
{
    // TODO: handle file path for subdirs
    struct File *curr = *root_dir;
    struct File *temp;
    struct Inode *inode, *cur, *tmp;
    int i = 0;
	int blocks = (filesize * KB_SIZE)/BLOCK_SIZE;
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
    struct File *new = (struct File*)malloc(sizeof(struct File)); 
    new->starting_inode = inode;
    new->filesize = filesize;
    strcpy(new->filename, filename);
    new->is_dir = is_dir;
    new->next = NULL;
    curr->next = new;
}

// Clears a file of all nodes/files
void empty(struct File** root_dir, struct Inode** head_ref)
{
	struct File *curr = *root_dir;
    struct File *temp;
    struct Inode *cur = *head_ref, *tmp;
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


/* -- Begin DF  ------ */

#define FS_SIZE_KB (16)
#define ROOT_PATH ("/")
#define MAGIC_NUM ((uint32_t) (UINT32_C(0xdeaddocs)))

typedef struct FileSystem {
    uint32_t magic;             // For denoting initialized mem
    size_t size;                // Total fs memspace size, in bytes
    struct Inode *head;         // Ptr to filesystem's header
    struct Inode *first_free;   // Free nodes list
    struct File *root;          // Ptr to filesystem's root director
} FileSystem;


/* -- init_fs -- */
// Initializes a filesystem and returns a ptr to it.
// Accepts:
//      size:   desired size of the filesystem, in kbs.
FileSystem *init_fs(size_t size) {
    // Validate given size
    size_t fs_sz = size * KB_SIZE;  // kb to bytes
    if (fs_sz % BLOCK_SIZE != 0) {
        printf("ERROR: Size must be block aligned.");
        return NULL;
    }

    // Build filesystem object
    FileSystem *fs = (FileSystem*)malloc(sizeof(FileSystem));	
    fs->magic = 8;
    fs->size = fs_sz;

    // Init fs head 
    struct Inode *head = (struct Inode*)malloc(sizeof(struct Inode));	
	head->inode_number = 0;
    head->block_start = malloc(BLOCK_SIZE);
    head->next = NULL;

    fs->head = head;
	
	// Intitialize inodes linked lists for filesystem
	// alocate inodes; start at one since head already exists
	int num_blocks = fs_sz / BLOCK_SIZE;
	for (int i = 1; i < num_blocks; i++) {
		push_inode(&head, i);
	}

	// Create root directory using one 4 KB block
	struct File *root = (struct File*)malloc(sizeof(struct File)); 
    root->starting_inode = head;
	head->is_free = 0;
    root->filesize = BLOCK_SIZE;
    strcpy(root->filename, ROOT_PATH);
    root->is_dir = 1;
    root->next = NULL;

    fs->root = root;

    return fs;
}

/* -- End DF    ----- */

/* Driver program to test above function */
int main() 
{
    printf("Initializing test filesystem...\n");
	FileSystem *fs = init_fs(FS_SIZE_KB);

	printf("Space free: %d KB\n", space_free(&fs->head));
	printf("Only 12 KB free since one node was used for root dir file...\n");
    
	// create 8KB file;
	char name[5];
	strcpy(name, "test");
	printf("Creating 8 KB file test in root dir will use two blocks...\n");
	create_file(&fs->root, &fs->head, 8, ROOT_PATH, "test", 0);
	// should be 4KB since the root dir uses one inode and two are used by new file
	printf("Space free: %d KB\n", space_free(&fs->head));
	printf("Only 4 KB free since one block was used for root dir file and two blocks for file test...\n");
	printf("Clearing filesystem for exit...\n");
	
    return 0; 
} 
