/* OHOS ABI: C allocation/resource hooks plus C++ allocation hooks, and the
 * mmap family. mmap/munmap/mremap are stripped from this manifest by CMake when
 * ENABLE_MMAP_HOOK_EXPORT is off (the OHOS default), so this one script covers
 * both the default and the opt-in build. */
{
global:
    malloc;
    free;
    calloc;
    realloc;
    aligned_alloc;
    memalign;
    posix_memalign;
    mmap;
    mmap64;
    munmap;
    mremap;
    ioctl;
    close;
    checkpoint;
    _Znwm;
    _Znwj;
    _Znam;
    _Znaj;
    _ZnwmRKSt9nothrow_t;
    _ZnwjRKSt9nothrow_t;
    _ZnamRKSt9nothrow_t;
    _ZnajRKSt9nothrow_t;
    _ZdlPv;
    _ZdaPv;
    _ZdlPvm;
    _ZdlPvj;
    _ZdaPvm;
    _ZdaPvj;
    _ZdlPvRKSt9nothrow_t;
    _ZdaPvRKSt9nothrow_t;
    _ZnwmSt11align_val_t;
    _ZnwjSt11align_val_t;
    _ZnamSt11align_val_t;
    _ZnajSt11align_val_t;
    _ZnwmSt11align_val_tRKSt9nothrow_t;
    _ZnwjSt11align_val_tRKSt9nothrow_t;
    _ZnamSt11align_val_tRKSt9nothrow_t;
    _ZnajSt11align_val_tRKSt9nothrow_t;
    _ZdlPvSt11align_val_t;
    _ZdaPvSt11align_val_t;
    _ZdlPvmSt11align_val_t;
    _ZdlPvjSt11align_val_t;
    _ZdaPvmSt11align_val_t;
    _ZdaPvjSt11align_val_t;
    _ZdlPvSt11align_val_tRKSt9nothrow_t;
    _ZdaPvSt11align_val_tRKSt9nothrow_t;

local: *;
};
