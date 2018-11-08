/* Internal function: used to get a block of memory, with its
   header 
*/
static __myfs_memory_block_t *__myfs_get_memory_block(__myfs_handle_t handle, size_t size) {
  __myfs_memory_block_t *curr, *prev, *new;
  
  if (handle == NULL) return NULL;
  if (size == ((size_t) 0)) return NULL;
  if (handle->free_memory == ((__myfs_off_t) 0)) return NULL;
  
  for (curr=((__myfs_memory_block_t *) __myfs_offset_to_ptr(handle, handle->free_memory)), prev=NULL;
       curr!=NULL;
       curr=((__myfs_memory_block_t *) __myfs_offset_to_ptr(handle, (prev=curr)->next))) {
    if (curr->size >= size) {
      if ((curr->size - size) < sizeof(__myfs_memory_block_t)) {
        if (prev == NULL) {
          handle->free_memory = curr->next;
        } else {
          prev->next = curr->next;
        }
        return curr;
      } else {
        new = (__myfs_memory_block_t *) (((void *) curr) + size);
        new->size = curr->size - size;
        new->next = curr->next;
        if (prev == NULL) {
          handle->free_memory = __myfs_ptr_to_offset(handle, new);
        } else {
          prev->next = __myfs_ptr_to_offset(handle, new);
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
static void __myfs_coalesce_free_memory_blocks(__myfs_handle_t handle, __myfs_memory_block_t *block) {
  __myfs_memory_block_t *clobbered, *next;
  
  if (handle == NULL) return;
  if (block == NULL) return;
  if (block->next == ((__myfs_off_t) 0)) return;
  if (((void *) (((void *) block) + block->size)) == ((void *) (__myfs_offset_to_ptr(handle, block->next)))) {
    clobbered = (__myfs_memory_block_t *) __myfs_offset_to_ptr(handle, block->next);
    block->size += clobbered->size;
    block->next = clobbered->next;
  }
  if (block->next == ((__myfs_off_t) 0)) return;
  if (((void *) (((void *) block) + block->size)) == ((void *) (__myfs_offset_to_ptr(handle, block->next)))) {
    clobbered = (__myfs_memory_block_t *) __myfs_offset_to_ptr(handle, block->next);
    block->size += clobbered->size;
    block->next = clobbered->next;
  }
  if (block->next == ((__myfs_off_t) 0)) return;  
  next = (__myfs_memory_block_t *) __myfs_offset_to_ptr(handle, block->next);
  if (next->next == ((__myfs_off_t) 0)) return;
  if (((void *) (((void *) next) + next->size)) == ((void *) (__myfs_offset_to_ptr(handle, next->next)))) {
    clobbered = (__myfs_memory_block_t *) __myfs_offset_to_ptr(handle, next->next);
    next->size += clobbered->size;
    next->next = clobbered->next;
  }  
}

/* Internal function: used to return a block of memory to the list of
   free blocks 
*/
static void __myfs_return_memory_block(__myfs_handle_t handle, __myfs_memory_block_t *block) {
  __myfs_memory_block_t *curr, *prev;
  
  if (handle == NULL) return;
  if (block == NULL) return;
  for (curr=((__myfs_memory_block_t *) __myfs_offset_to_ptr(handle, handle->free_memory)), prev=NULL;
       curr!=NULL;
       curr=((__myfs_memory_block_t *) __myfs_offset_to_ptr(handle, (prev=curr)->next))) {
    if (((void *) block) < ((void *) curr))
      break;
  }
  if (prev == NULL) {
    block->next = handle->free_memory;
    handle->free_memory = __myfs_ptr_to_offset(handle, block);
    __myfs_coalesce_free_memory_blocks(handle, block);
  } else {
    block->next = __myfs_ptr_to_offset(handle, curr);
    prev->next = __myfs_ptr_to_offset(handle, block);
    __myfs_coalesce_free_memory_blocks(handle, prev);
  }
}

/* A function to free memory in the style of free(), using handle to
   describe the filesystem and an offset instead of a pointer.
*/
static void __myfs_free_memory(__myfs_handle_t handle, __myfs_off_t off) {
  void *ptr;
  __myfs_memory_block_t *block;
  
  if (handle == NULL) return;
  if (off == ((__myfs_off_t) 0)) return;
  ptr = ((void *) __myfs_offset_to_ptr(handle, off)) - sizeof(__myfs_memory_block_t);
  block = (__myfs_memory_block_t *) ptr;
  __myfs_return_memory_block(handle, block);
}

/* A function allocate memory in the style of malloc(), using handle
   to describe the filesystem and returning an offset instead of a
   pointer.
*/
static __myfs_off_t __myfs_allocate_memory(__myfs_handle_t handle, size_t size) {
  size_t s;
  __myfs_memory_block_t *ptr;
  
  if (handle == NULL) return ((__myfs_off_t) 0);
  if (size == ((size_t) 0)) return ((__myfs_off_t) 0);
  s = size + sizeof(__myfs_memory_block_t);
  if (s < size) return ((__myfs_off_t) 0);
  ptr = __myfs_get_memory_block(handle, s);
  if (ptr == NULL) return ((__myfs_off_t) 0);
  ptr->user_size = size;
  return __myfs_ptr_to_offset(handle, ((void *) ptr) + sizeof(__myfs_memory_block_t));
}

/* A function reallocate memory in the style of realloc(), using
   handle to describe the filesystem and returning/using offsets
   instead of pointers.
*/
static __myfs_off_t __myfs_reallocate_memory(__myfs_handle_t handle, __myfs_off_t old_off, size_t new_size) {
  __myfs_off_t new_off;
  void *old_ptr, *new_ptr;
  size_t copy_size, ns;
  __myfs_memory_block_t *old_block;
  __myfs_memory_block_t *trash;
  
  if (handle == NULL) return ((__myfs_off_t) 0);
  if (new_size == ((size_t) 0)) {
    __myfs_free_memory(handle, old_off);
    return ((__myfs_off_t) 0);
  }
  old_ptr = __myfs_offset_to_ptr(handle, old_off);
  old_block = (__myfs_memory_block_t *) (old_ptr - sizeof(__myfs_memory_block_t));
  ns = new_size + sizeof(__myfs_memory_block_t);
  if (ns >= new_size) {
    if ((ns + sizeof(__myfs_memory_block_t)) >= ns) {
      if ((old_block->size) >= (ns + sizeof(__myfs_memory_block_t))) {
        trash = (__myfs_memory_block_t *) (((void *) old_block) + ns);
        trash->size = old_block->size - ns;
        trash->user_size = ((size_t) 0);
        trash->next = ((__myfs_off_t) 0);
        __myfs_return_memory_block(handle, trash);
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
  new_off = __myfs_allocate_memory(handle, new_size);
  if (new_off == ((__myfs_off_t) 0)) return ((__myfs_off_t) 0);
  new_ptr = __myfs_offset_to_ptr(handle, new_off);
  copy_size = old_block->user_size;
  if (new_size < copy_size) copy_size = new_size;
  memcpy(new_ptr, old_ptr, copy_size);
  __myfs_free_memory(handle, old_off);
  return new_off;
}

/* A function returning the maximum size one can request from the
   memory allocation functions and get the request satisfied.
*/
static size_t __myfs_maximum_free_size(__myfs_handle_t handle) {
  size_t size;
  __myfs_memory_block_t *curr;
  
  if (handle == NULL) return ((size_t) 0);
  if (handle->free_memory == ((__myfs_off_t) 0)) return ((size_t) 0);  
  size = (size_t) 0;
  for (curr=((__myfs_memory_block_t *) __myfs_offset_to_ptr(handle, handle->free_memory));
       curr!=NULL;
       curr=((__myfs_memory_block_t *) __myfs_offset_to_ptr(handle, curr->next))) {
    if (curr->size > size) {
      size = curr->size;
    }
  }
  if (size < sizeof(__myfs_memory_block_t)) return ((size_t) 0);
  return size - sizeof(__myfs_memory_block_t);
}
