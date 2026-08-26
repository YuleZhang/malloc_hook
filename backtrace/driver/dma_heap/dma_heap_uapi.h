#ifndef ALLOC_HOOK_DRIVER_DMA_HEAP_UAPI_H
#define ALLOC_HOOK_DRIVER_DMA_HEAP_UAPI_H

// Vendored copy of the kernel's linux/dma-heap.h UAPI.
//
// Cross toolchains frequently ship a sysroot without this header even when the
// target kernel exports /dev/dma_heap, so resource tracking cannot depend on it
// being installed. This mirrors the existing vendored ion/kgsl/midgard UAPI
// headers. The ioctl number and struct layout are kernel ABI and therefore
// stable; do not change them.

#include <linux/ioctl.h>
#include <linux/types.h>

struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};

#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC \
    _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)

#endif  // ALLOC_HOOK_DRIVER_DMA_HEAP_UAPI_H
