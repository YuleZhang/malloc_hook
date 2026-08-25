#pragma once
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <cstddef>

extern void* (*m_sys_malloc)(size_t);
extern void (*m_sys_free)(void*);
extern void* (*m_sys_calloc)(size_t, size_t);
extern void* (*m_sys_realloc)(void*, size_t);
extern void* (*m_sys_memalign)(size_t, size_t);
extern void* (*m_sys_aligned_alloc)(size_t, size_t);
extern int (*m_sys_posix_memalign)(void**, size_t, size_t);
extern void* (*m_sys_mmap)(void*, size_t, int, int, int, off_t);
extern int (*m_sys_munmap)(void*, size_t);
extern void* (*m_sys_mremap)(void*, size_t, size_t, int, ...);
#if !defined(mmap64)
extern void* (*m_sys_mmap64)(void*, size_t, int, int, int, off_t);
#endif
