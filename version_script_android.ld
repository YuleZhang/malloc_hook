/* Android hook ABI. android_fdsan_close_with_tag is intentionally exported
 * without a local implementation detail suffix: Bionic's dynamic linker allows
 * an LD_PRELOAD definition to satisfy the versioned weak libc reference. The
 * Android build gate and device regression both verify that contract. */
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
