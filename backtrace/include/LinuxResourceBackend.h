#pragma once

// Linux resource UAPI is intentionally isolated behind this optional adapter.
// Generic Linux builds compile without the header or any vendor resource
// interception; Android/OHOS resource backends retain their existing driver
// contracts.
#if MALLOC_HOOK_ENABLE_DMA_CAPTURE && \
    (defined(MALLOC_HOOK_TARGET_OS_LINUX) || \
     defined(MALLOC_HOOK_TARGET_OS_ANDROID) || \
     defined(MALLOC_HOOK_TARGET_OS_OHOS))
// Prefer the sysroot's UAPI when present; fall back to the vendored copy so a
// cross toolchain without linux/dma-heap.h can still track /dev/dma_heap.
#if defined(MALLOC_HOOK_HAVE_DMA_HEAP_UAPI) && MALLOC_HOOK_HAVE_DMA_HEAP_UAPI
#include <linux/dma-heap.h>
#else
#include "driver/dma_heap/dma_heap_uapi.h"
#endif
#endif
