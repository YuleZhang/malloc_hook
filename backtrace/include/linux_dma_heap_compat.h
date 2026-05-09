#ifndef BACKTRACE_LINUX_DMA_HEAP_COMPAT_H_
#define BACKTRACE_LINUX_DMA_HEAP_COMPAT_H_

#if defined(__has_include)
#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#else
#include <fcntl.h>
#include <linux/ioctl.h>
#include <linux/types.h>

#define DMA_HEAP_VALID_FD_FLAGS (O_CLOEXEC | O_ACCMODE)
#define DMA_HEAP_VALID_HEAP_FLAGS (0)

struct dma_heap_allocation_data {
  __u64 len;
  __u32 fd;
  __u32 fd_flags;
  __u64 heap_flags;
};

#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#endif
#else
#include <linux/dma-heap.h>
#endif

#endif  // BACKTRACE_LINUX_DMA_HEAP_COMPAT_H_
