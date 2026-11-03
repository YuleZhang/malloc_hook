# Standalone device diagnostic: does not link against liballoc_hook.so.
# Can also be configured independently on a glibc Linux development host.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.16)
  project(alloc_hook_probe LANGUAGES CXX)
  include(CTest)
endif()

add_executable(alloc_hook_probe alloc_hook_probe.cpp)
target_compile_features(alloc_hook_probe PRIVATE cxx_std_17)
if(DEFINED MALLOC_HOOK_TARGET_OS)
  target_compile_definitions(alloc_hook_probe PRIVATE
    "ALLOC_PROBE_TARGET_OS=\"${MALLOC_HOOK_TARGET_OS}\"")
endif()
target_include_directories(alloc_hook_probe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/driver/ion
  ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/driver/dma_heap)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(alloc_hook_probe PRIVATE -Wall -Wextra -Werror)
endif()
install(TARGETS alloc_hook_probe RUNTIME DESTINATION out/bin)

if(BUILD_TESTING AND (NOT DEFINED MALLOC_HOOK_BUILD_TESTS OR MALLOC_HOOK_BUILD_TESTS))
  add_test(NAME alloc_hook_probe_inspect COMMAND alloc_hook_probe inspect)
  set_tests_properties(alloc_hook_probe_inspect PROPERTIES
    PASS_REGULAR_EXPRESSION "\"kind\":\"capability\"")
endif()
