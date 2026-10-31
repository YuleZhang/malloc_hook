/* Android hook ABI. Same C hook set as Linux, but fcntl64 is absent (Bionic has
 * no such symbol) and android_fdsan_close_with_tag is added: Bionic's dynamic
 * linker lets an LD_PRELOAD definition satisfy the versioned weak libc
 * reference, which is what lets the hook see accelerator libraries that close
 * their descriptors through fdsan. */
{
global:
    malloc;
    free;
    fork;
    calloc;
    realloc;
    aligned_alloc;
    memalign;
    posix_memalign;
    mmap;
    munmap;
    mremap;
    ioctl;
    close;
    dup;
    dup2;
    dup3;
    fcntl;
    android_fdsan_close_with_tag;
    mmap64;
    checkpoint;

local: *;
};
