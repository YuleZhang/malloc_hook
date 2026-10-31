/* Generic Linux hook ABI. Shares the C hook set with Android and adds fcntl64,
 * which is where glibc redirects F_DUPFD* when a large-file build is in effect.
 * No android_fdsan_close_with_tag: libc on this platform has no such symbol. */
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
    fcntl64;
    mmap64;
    checkpoint;

local: *;
};
