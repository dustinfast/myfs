/*
* File system in memory test structure/design.
* Author: Joel Keller
* Date: 10/5/2018

* Usage: 
    gcc -o fs jdktest.c -03
    ./fs

*/
#include<stdio.h> 
#include<stdlib.h> 
#include <string.h>

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
    int free;

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
    int dir;
    //next file in folder or first if dir
    struct File *next;
};

/* Function to add a inode at the end of Filesystem Linked List.
To be used to initialize all the inodes for the disk size */
void push_inode(struct Inode** head_ref, int number) 
{
    struct Inode *curr = *head_ref;
    struct Inode *first, *temp;
	while (curr->next != NULL) {
		temp = curr->next;
        curr = temp;
	}
    // Allocate memory for inode eventually we will be using space from the memory he gives us for the filesystem
    struct Inode* new_node = (struct Inode*)malloc(sizeof(struct Inode)); 

    new_node->inode_number = number;
    new_node->block_start = malloc(BLOCK_SIZE);
    new_node->free = 1;
    new_node->next = NULL; 
	curr->next = new_node;
}

//Might be used to find space for new files
void *find_x_blocks_free(struct Inode** head_ref, int x)
{
    struct Inode *curr = *head_ref;
    struct Inode *first, *temp;
    int i = 0;
    while (curr->next != NULL) {
        if (curr->free) {
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
        if (curr->free == 1) {
			printf("Block %d is free.\n", curr->inode_number);
            i++;
        }
        temp = curr->next;
        curr = temp;
    }
    return (i * BLOCK_SIZE) / 1024; // free space in KB
}

//Create file at the end of dir
void create_file(struct File** root_dir, struct Inode** head_ref, int filesize, char* path, char *filename, int dir)
{
    // TODO: handle file path for subdirs
    struct File *curr = *root_dir;
    struct File *temp;
    struct Inode *inode, *cur, *tmp;
    int i = 0;
	int blocks = (filesize * 1024)/BLOCK_SIZE;
    //find last file in dir
    while (curr->next != NULL) {
        temp = curr->next;
        curr = temp;
    }
    inode = find_x_blocks_free(head_ref, blocks);
	cur = inode;
	
	for (i = 0; i < blocks; i++) {
		cur->free = 0;		
		tmp = cur->next;
		cur = tmp;
	}
    // Allocate memory for File eventually we will be using space from the memory he gives us for the filesystem
    struct File *new = (struct File*)malloc(sizeof(struct File)); 
    new->starting_inode = inode;
    new->filesize = filesize;
    strcpy(new->filename, filename);
    new->dir = dir;
    new->next = NULL;
    curr->next = new;
}

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

/* Driver program to test above function */
int main() 
{
	char path[1];
	path[0] = '/';

	printf("Initializing 16KB test filesystem... Only accessible to this program...\n");
	int filesyssize = 16 * 1024;  // Filesystem size
    
    // Allocate memory for head 
    // (eventually we will be using space from the memory he gives us for the filesystem
    struct Inode* head = (struct Inode*)malloc(sizeof(struct Inode));	
	head->inode_number = 0;
    head->block_start = malloc(BLOCK_SIZE);
    head->next = NULL;
	
	// Intitialize inodes linked lists for filesystem
	// alocate inodes; start at one since head already exists
	int num_blocks = filesyssize / BLOCK_SIZE;
	for (int i = 1; i < num_blocks; i++) {
		push_inode(&head, i);
	}
	printf("Creating root directory using one %d KB block.\n", BLOCK_SIZE/1024);
	// Allocate memory for root dir 
    // eventually we will be using space from the memory he gives us
    struct File *root = (struct File*)malloc(sizeof(struct File)); 
    root->starting_inode = head;
	head->free = 0;
    root->filesize = BLOCK_SIZE;
    strcpy(root->filename, path);
    root->dir = 1;
    root->next = NULL;
	// should be 12KB since the root dir uses one inode 
	printf("Space free: %d KB\n", space_free(&head));
	printf("Only 12 KB free since one block was used for root dir file...\n");
	// create 8KB file;
	char name[5];
	strcpy(name, "test");
	printf("Creating 8 KB file test in root dir will use too blocks...\n");
	create_file(&root, &head, 8, path, name, 0);
	// should be 4KB since the root dir uses one inode and too are used by new file
	printf("Space free: %d KB\n", space_free(&head));
	printf("Only 4 KB free since one block was used for root dir file and two blocks for file test...\n");
	printf("Clearing filesystem for exit...\n");
	
    return 0; 
} 
