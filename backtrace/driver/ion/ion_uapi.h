#pragma once

#include <linux/ioctl.h>
#include <linux/types.h>

#include <cstddef>

using ion_user_handle_t = int;

struct ion_allocation_data {
    size_t len;
    size_t align;
    unsigned int heap_id_mask;
    unsigned int flags;
    ion_user_handle_t handle;
};

struct ion_fd_data {
    ion_user_handle_t handle;
    int fd;
};

struct ion_handle_data {
    ion_user_handle_t handle;
};

struct ion_new_allocation_data {
    __u64 len;
    __u32 heap_id_mask;
    __u32 flags;
    __u32 fd;
    __u32 unused;
};

#define ALLOC_HOOK_ION_IOC_MAGIC 'I'
#define ALLOC_HOOK_ION_IOC_ALLOC \
    _IOWR(ALLOC_HOOK_ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ALLOC_HOOK_ION_IOC_FREE \
    _IOWR(ALLOC_HOOK_ION_IOC_MAGIC, 1, struct ion_handle_data)
#define ALLOC_HOOK_ION_IOC_MAP \
    _IOWR(ALLOC_HOOK_ION_IOC_MAGIC, 2, struct ion_fd_data)
#define ALLOC_HOOK_ION_IOC_SHARE \
    _IOWR(ALLOC_HOOK_ION_IOC_MAGIC, 4, struct ion_fd_data)
#define ALLOC_HOOK_ION_IOC_IMPORT \
    _IOWR(ALLOC_HOOK_ION_IOC_MAGIC, 5, struct ion_fd_data)
#define ALLOC_HOOK_ION_IOC_NEW_ALLOC \
    _IOWR(ALLOC_HOOK_ION_IOC_MAGIC, 0, struct ion_new_allocation_data)
