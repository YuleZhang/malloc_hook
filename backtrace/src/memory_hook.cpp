#include "memory_hook.h"

void* (*m_sys_malloc)(size_t) = nullptr;
void (*m_sys_free)(void*) = nullptr;
void* (*m_sys_calloc)(size_t, size_t) = nullptr;
void* (*m_sys_realloc)(void*, size_t) = nullptr;
void* (*m_sys_memalign)(size_t, size_t) = nullptr;
void* (*m_sys_aligned_alloc)(size_t, size_t) = nullptr;
int (*m_sys_posix_memalign)(void**, size_t, size_t) = nullptr;
void* (*m_sys_mmap)(void*, size_t, int, int, int, off_t) = nullptr;
int (*m_sys_munmap)(void*, size_t) = nullptr;
void* (*m_sys_mremap)(void*, size_t, size_t, int, ...) = nullptr;
#if !defined(mmap64)
void* (*m_sys_mmap64)(void*, size_t, int, int, int, off_t) = nullptr;
#endif
