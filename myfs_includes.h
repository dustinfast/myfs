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
