/* Android and generic Linux currently share this documented C hook ABI.
 * Platform selection remains explicit in CMake so either policy can diverge
 * without changing capture or bookkeeping code. */
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
