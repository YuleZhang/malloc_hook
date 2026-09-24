cmake_minimum_required(VERSION 3.23)

# Package version: the VERSION file is the source of truth (tag it as v<VERSION>);
# override with -DMALLOC_HOOK_VERSION=... for a one-off build.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION _MALLOC_HOOK_VERSION_FILE)
set(MALLOC_HOOK_VERSION "${_MALLOC_HOOK_VERSION_FILE}" CACHE STRING "malloc_hook package version")

project(malloc_hook VERSION ${MALLOC_HOOK_VERSION} LANGUAGES C CXX ASM)

option(MALLOC_HOOK_BUILD_TESTS "Build malloc_hook test binaries" ON)
option(MALLOC_HOOK_OHOS_MMAP_HOOK "Export mmap/munmap hooks in OHOS builds" OFF)

if(OHOS)
  if(MALLOC_HOOK_OHOS_MMAP_HOOK)
    set(VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/version_script_ohos_mmap.ld)
  else()
    set(VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/version_script_ohos.ld)
  endif()
else()
  set(VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/version_script.ld)
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O3 -fPIC")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(ANDROID OR OHOS)
  # now old ANDROID have libc++_shared.so, new ANDROID have libc++.so so we use
  # libc++_static by default, by set ANDROID_STL=c++_static only take effect on old NDK,
  # so we force config by use xxx_LINKER_FLAGS
  message(STATUS "config static-libstdc++ for ANDROID or OHOS")
  set(CMAKE_EXE_LINKER_FLAGS "-fuse-ld=lld")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static-libstdc++")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -static-libstdc++")
endif()
# Check if using Clang
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--version-script=${VERSION_SCRIPT}")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

add_subdirectory(unwindstack/cmake)
add_subdirectory(backtrace)

# 添加 .so
add_library(alloc_hook SHARED ${CMAKE_SOURCE_DIR}/src/alloc_hook.cpp)
# 链接需要的库和头文件
target_link_libraries(alloc_hook PRIVATE helper)
# 安装到 out/lib 目录
install(TARGETS alloc_hook DESTINATION ${CMAKE_INSTALL_PREFIX}/out/lib)

if (MALLOC_HOOK_BUILD_TESTS)
  if (TARGET gtest)
    message(STATUS "gtest is already defined")
  else()
    set(GOOGLETEST_VERSION 1.15.2)
    add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/googletest/googletest)
    add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/opencl_stub)
    add_subdirectory(test)
  endif()
endif()

# copy database compile_commands.json to PROJECT_SOURCE_DIR
add_custom_target(
  copy_database_compile_commands_mem_trace ALL
  COMMAND
    ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_BINARY_DIR}/compile_commands.json
    ${PROJECT_SOURCE_DIR}/compile_commands.json)

# --------------------------------------------------------------------------- #
# Distribution package (cpack): a self-contained tarball with the hook, the
# Python trace tooling and the docs, for quick sharing and use.
#   cmake --build <build> --target package     # or: (cd <build> && cpack)
# Produces malloc_hook-<version>-<abi>.tar.gz laid out as:
#   lib/liballoc_hook.so  scripts/*.py  scripts/maps.example.json  README.md  VERSION
# The `dist` component is EXCLUDE_FROM_ALL so a plain `ninja install` (used by
# build_android.sh) does not scatter these into the source tree.
# --------------------------------------------------------------------------- #
install(TARGETS alloc_hook LIBRARY DESTINATION lib COMPONENT dist EXCLUDE_FROM_ALL)
install(FILES
    ${CMAKE_SOURCE_DIR}/scripts/process_memory_stack.py
    ${CMAKE_SOURCE_DIR}/scripts/build_perfetto_alloc_track.py
    ${CMAKE_SOURCE_DIR}/scripts/perfetto_proto.py
    ${CMAKE_SOURCE_DIR}/scripts/maps.example.json
    DESTINATION scripts COMPONENT dist EXCLUDE_FROM_ALL)
install(FILES ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/VERSION
    DESTINATION . COMPONENT dist EXCLUDE_FROM_ALL)

set(CPACK_PACKAGE_NAME "malloc_hook")
set(CPACK_PACKAGE_VERSION "${MALLOC_HOOK_VERSION}")
set(CPACK_GENERATOR "TGZ")
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL dist)
if(DEFINED ANDROID_ABI)
  set(_MH_ABI "${ANDROID_ABI}")
else()
  set(_MH_ABI "${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(CPACK_ARCHIVE_DIST_FILE_NAME "malloc_hook-${MALLOC_HOOK_VERSION}-${_MH_ABI}")
set(CPACK_PACKAGE_FILE_NAME "malloc_hook-${MALLOC_HOOK_VERSION}-${_MH_ABI}")
include(CPack)
