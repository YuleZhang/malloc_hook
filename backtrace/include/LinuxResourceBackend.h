#pragma once

// Linux resource UAPI is intentionally isolated behind this optional adapter.
// Generic Linux builds compile without the header or any vendor resource
// interception; Android/OHOS resource backends retain their existing driver
// contracts.
#if MALLOC_HOOK_ENABLE_RESOURCE_TRACKING && \
    (defined(MALLOC_HOOK_TARGET_OS_LINUX) || \
     defined(MALLOC_HOOK_TARGET_OS_ANDROID) || \
     defined(MALLOC_HOOK_TARGET_OS_OHOS))
#include <linux/dma-heap.h>
#endif
