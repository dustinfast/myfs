/*
a4_fs definitions - A collection of definitions and helper funcs for a4_fs.c

Contributors: Dustin Fast
*/


/* Begin Definitions ---------------------------------------------------- */


#define BYTES_IN_KB (1024)  // Num bytes in a kb


/* End Definitions ------------------------------------------------------- */
/* Begin Utility helpers ------------------------------------------------- */


/* Returns a size_t denoting the given null-terminated string's length. */
size_t str_len(char *arr) {
    int length = 0;
    for (char *c = arr; *c != '\0'; c++)
        length++;

    return length;
}

/* Writes the string given by arr to stdout. */
void str_write(char *arr) {
    size_t total_written = 0;
    size_t char_count = str_len(arr);

    // Write string to stdout
    while (total_written < char_count)
        total_written += write(fileno(stdout), arr + total_written, char_count - total_written);
}

/* Returns the given number of kilobytes converted to bytes. */
size_t kb_to_bytes(size_t size) {
    return (size * BYTES_IN_KB);
}

/* Returns the given number of bytes converted to kilobytes.  */
size_t bytes_to_kb(size_t size) {
    return (size / BYTES_IN_KB);
}

/* Returns 1 if given bytes are alignable on the given block_sz, else 0. */
int is_bytes_blockalignable(size_t bytes, size_t block_sz) {
    if (bytes % block_sz == 0)
        return 1;
    return 0;
}

/* Returns 1 if given size is alignable on the given block_sz, else 0. */
int is_kb_blockaligned(size_t kbs_size, size_t block_sz) {
    return is_bytes_blockalignable(kb_to_bytes(kbs_size), block_sz);
}


/* End Utility helpers --------------------------------------------------- */




// ----- Snippets provided by CLauter

/* Internal function: used to get a block of memory, with its
   header 
*/
static MemBlock *get_memory_block(FileSystem* fs, size_t size) {
  MemBlock *curr, *prev, *new;
  
  if (fs == NULL) return NULL;
  if (size == 0) return NULL;
  if (fs->first_free == ((__his_off_t) 0)) return NULL;
  
  for (curr=((MemBlock *) get_offset_to_ptr(fs, fs->first_free)), prev=NULL;
       curr!=NULL;
       curr=((MemBlock *) get_offset_to_ptr(fs, (prev=curr)->next))) {
    if (curr->size >= size) {
      if ((curr->size - size) < sizeof(MemBlock)) {
        if (prev == NULL) {
          fs->first_free = curr->next;
        } else {
          prev->next = curr->next;
        }
        return curr;
      } else {
        new = (MemBlock *) (((void *) curr) + size);
        new->size = curr->size - size;
        new->next = curr->next;
        if (prev == NULL) {
          fs->first_free = get_ptr_to_offset(fs, new);
        } else {
          prev->next = get_ptr_to_offset(fs, new);
        }
        curr->size = size;
        return curr;
      }
    }
  }
  return NULL;
}

/* Internal function: used to coalesce adjacent free memory blocks 
   in the ordered linked list of free blocks.
*/
static void coalesce_free_memory_blocks(FileSystem* fs, MemBlock *block) {
  MemBlock *clobbered, *next;
  
  if (fs == NULL) return;
  if (block == NULL) return;
  if (block->next == ((__his_off_t) 0)) return;
  if (((void *) (((void *) block) + block->size)) == ((void *) (get_offset_to_ptr(fs, block->next)))) {
    clobbered = (MemBlock *) get_offset_to_ptr(fs, block->next);
    block->size += clobbered->size;
    block->next = clobbered->next;
  }
  if (block->next == ((__his_off_t) 0)) return;
  if (((void *) (((void *) block) + block->size)) == ((void *) (get_offset_to_ptr(fs, block->next)))) {
    clobbered = (MemBlock *) get_offset_to_ptr(fs, block->next);
    block->size += clobbered->size;
    block->next = clobbered->next;
  }
  if (block->next == ((__his_off_t) 0)) return;  
  next = (MemBlock *) get_offset_to_ptr(fs, block->next);
  if (next->next == ((__his_off_t) 0)) return;
  if (((void *) (((void *) next) + next->size)) == ((void *) (get_offset_to_ptr(fs, next->next)))) {
    clobbered = (MemBlock *) get_offset_to_ptr(fs, next->next);
    next->size += clobbered->size;
    next->next = clobbered->next;
  }  
}

/* Internal function: used to return a block of memory to the list of
   free blocks 
*/
static void return_memory_block(FileSystem* fs, MemBlock *block) {
  MemBlock *curr, *prev;
  
  if (fs == NULL) return;
  if (block == NULL) return;
  for (curr=((MemBlock *) get_offset_to_ptr(fs, fs->first_free)), prev=NULL;
       curr!=NULL;
       curr=((MemBlock *) get_offset_to_ptr(fs, (prev=curr)->next))) {
    if (((void *) block) < ((void *) curr))
      break;
  }
  if (prev == NULL) {
    block->next = fs->first_free;
    fs->first_free = get_ptr_to_offset(fs, block);
    coalesce_free_memory_blocks(fs, block);
  } else {
    block->next = get_ptr_to_offset(fs, curr);
    prev->next = get_ptr_to_offset(fs, block);
    coalesce_free_memory_blocks(fs, prev);
  }
}

/* A function to free memory in the style of free(), using fs to
   describe the filesystem and an offset instead of a pointer.
*/
static void free_memory(FileSystem* fs, __his_off_t off) {
  void *ptr;
  MemBlock *block;
  
  if (fs == NULL) return;
  if (off == ((__his_off_t) 0)) return;
  ptr = ((void *) get_offset_to_ptr(fs, off)) - sizeof(MemBlock);
  block = (MemBlock *) ptr;
  return_memory_block(fs, block);
}

/* A function allocate memory in the style of malloc(), using fs
   to describe the filesystem and returning an offset instead of a
   pointer.
*/
static __his_off_t allocate_memory(FileSystem* fs, size_t size) {
  size_t s;
  MemBlock *ptr;
  
  if (fs == NULL) return ((__his_off_t) 0);
  if (size == 0) return ((__his_off_t) 0);
  s = size + sizeof(MemBlock);
  if (s < size) return ((__his_off_t) 0);
  ptr = get_memory_block(fs, s);
  if (ptr == NULL) return ((__his_off_t) 0);
  ptr->user_size = size;
  return get_ptr_to_offset(fs, ((void *) ptr) + sizeof(MemBlock));
}

/* A function reallocate memory in the style of realloc(), using
   fs to describe the filesystem and returning/using offsets
   instead of pointers.
*/
static __his_off_t reallocate_memory(FileSystem* fs, __his_off_t old_off, size_t new_size) {
  __his_off_t new_off;
  void *old_ptr, *new_ptr;
  size_t copy_size, ns;
  MemBlock *old_block;
  MemBlock *trash;
  
  if (fs == NULL) return ((__his_off_t) 0);
  if (new_size == 0) {
    free_memory(fs, old_off);
    return ((__his_off_t) 0);
  }
  old_ptr = get_offset_to_ptr(fs, old_off);
  old_block = (MemBlock *) (old_ptr - sizeof(MemBlock));
  ns = new_size + sizeof(MemBlock);
  if (ns >= new_size) {
    if ((ns + sizeof(MemBlock)) >= ns) {
      if ((old_block->size) >= (ns + sizeof(MemBlock))) {
        trash = (MemBlock *) (((void *) old_block) + ns);
        trash->size = old_block->size - ns;
        trash->user_size = 0;
        trash->next = ((__his_off_t) 0);
        return_memory_block(fs, trash);
        old_block->size = ns;
        old_block->user_size = new_size;
        return old_off;
      }
    }
  }
  if (new_size < old_block->user_size) {
    old_block->user_size = new_size;
    return old_off;
  }
  new_off = allocate_memory(fs, new_size);
  if (new_off == ((__his_off_t) 0)) return ((__his_off_t) 0);
  new_ptr = get_offset_to_ptr(fs, new_off);
  copy_size = old_block->user_size;
  if (new_size < copy_size) copy_size = new_size;
  memcpy(new_ptr, old_ptr, copy_size);
  free_memory(fs, old_off);
  return new_off;
}

/* A function returning the maximum size one can request from the
   memory allocation functions and get the request satisfied.
*/
static size_t maximum_free_size(FileSystem* fs) {
  size_t size;
  MemBlock *curr;
  
  if (fs == NULL) return 0;
  if (fs->first_free == ((__his_off_t) 0)) return 0;  
  size = (size_t) 0;
  for (curr=((MemBlock *) get_offset_to_ptr(fs, fs->first_free));
       curr!=NULL;
       curr=((MemBlock *) get_offset_to_ptr(fs, curr->next))) {
    if (curr->size > size) {
      size = curr->size;
    }
  }
  if (size < sizeof(MemBlock)) return 0;
  return size - sizeof(MemBlock);
}

