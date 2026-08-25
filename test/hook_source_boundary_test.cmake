cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED REPO_ROOT)
  message(FATAL_ERROR "REPO_ROOT is required")
endif()

file(READ "${REPO_ROOT}/src/alloc_hook.cpp" source)
foreach(route IN ITEMS
    "AllocHook::inst().malloc(size)"
    "AllocHook::inst().free(ptr)"
    "AllocHook::inst().mmap(addr, size, prot, flags, fd, offset)"
    "AllocHook::inst().munmap(addr, size)"
    "AllocHook::inst().mremap(old_addr, old_size, new_size, flags, new_addr)"
    "AllocHook::inst().ioctl(fd, request_value, arg)"
    "AllocHook::inst().close(fd)")
  string(FIND "${source}" "${route}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing unified hook-source route: ${route}")
  endif()
endforeach()

foreach(contract IN ITEMS
    "direct syscalls are"
    "success-only"
    "Release paths reuse stored identity")
  string(FIND "${source}" "${contract}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing hook-source contract documentation: ${contract}")
  endif()
endforeach()

message(STATUS "Unified hook-source boundary checks passed")
