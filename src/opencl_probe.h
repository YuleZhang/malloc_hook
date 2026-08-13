#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* malloc_hook_opencl_maybe_wrap(const char* name, void* real_symbol);
void malloc_hook_opencl_get_snapshot(size_t* live_requested_bytes, const char** last_api);

#ifdef __cplusplus
}
#endif
