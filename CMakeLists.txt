cmake_minimum_required(VERSION 3.23)

project(malloc_hook LANGUAGES C CXX ASM)

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
