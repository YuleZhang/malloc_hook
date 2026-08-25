cmake_minimum_required(VERSION 3.23)

project(malloc_hook LANGUAGES C CXX ASM)

option(MALLOC_HOOK_BUILD_TESTS "Build malloc_hook test binaries" ON)
option(MALLOC_HOOK_OHOS_MMAP_HOOK "Export mmap/munmap hooks in OHOS builds" OFF)
set(_malloc_hook_default_resource_tracking OFF)
if(ANDROID OR OHOS)
  set(_malloc_hook_default_resource_tracking ON)
endif()
option(MALLOC_HOOK_ENABLE_RESOURCE_TRACKING
  "Build DMA/ION/GPU resource tracking"
  ${_malloc_hook_default_resource_tracking})
option(MALLOC_HOOK_BUILD_GL_TESTS
  "Build the Android OpenGL integration test fixture"
  ${ANDROID})
set(MALLOC_HOOK_ANDROID_TOOLCHAIN_FILE ""
    CACHE FILEPATH "Optional Android NDK CMake toolchain for cross-build gates")
set(MALLOC_HOOK_OHOS_TOOLCHAIN_FILE ""
    CACHE FILEPATH "Optional OHOS CMake toolchain for cross-build gates")

include(CheckCXXSourceCompiles)
include(CTest)

# Keep OS, libc, and CPU identity independent. In particular, musl is not an
# OHOS identifier and non-Bionic is not a generic Linux/glibc identifier.
if(ANDROID AND OHOS)
  message(FATAL_ERROR "Android and OHOS target identities are mutually exclusive")
elseif(ANDROID)
  set(MALLOC_HOOK_TARGET_OS android)
  check_cxx_source_compiles(
    "#include <sys/cdefs.h>\n#ifndef __BIONIC__\n#error Android requires Bionic\n#endif\nint main() { return 0; }"
    MALLOC_HOOK_TARGET_USES_BIONIC)
  if(NOT MALLOC_HOOK_TARGET_USES_BIONIC)
    message(FATAL_ERROR "Android target selected without a Bionic toolchain")
  endif()
  set(MALLOC_HOOK_TARGET_LIBC bionic)
elseif(OHOS)
  set(MALLOC_HOOK_TARGET_OS ohos)
  check_cxx_source_compiles(
    "#include <sys/cdefs.h>\n#ifndef __MUSL__\n#error OHOS support requires the NDK musl marker\n#endif\nint main() { return 0; }"
    MALLOC_HOOK_TARGET_USES_MUSL)
  if(NOT MALLOC_HOOK_TARGET_USES_MUSL)
    message(FATAL_ERROR "OHOS target selected without the expected musl toolchain")
  endif()
  set(MALLOC_HOOK_TARGET_LIBC musl)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(MALLOC_HOOK_TARGET_OS linux)
  check_cxx_source_compiles(
    "#include <features.h>\n#ifndef __GLIBC__\n#error Generic Linux support currently requires glibc\n#endif\nint main() { return 0; }"
    MALLOC_HOOK_TARGET_USES_GLIBC)
  if(NOT MALLOC_HOOK_TARGET_USES_GLIBC)
    message(FATAL_ERROR
      "Generic Linux currently supports glibc only; musl Linux is not treated as OHOS")
  endif()
  set(MALLOC_HOOK_TARGET_LIBC glibc)
else()
  message(FATAL_ERROR
    "Unsupported target OS '${CMAKE_SYSTEM_NAME}'; expected Android, OHOS, or Linux")
endif()

if(NOT MALLOC_HOOK_ENABLE_RESOURCE_TRACKING)
  set(MALLOC_HOOK_EXPORT_RESOURCE_HOOKS 0)
endif()

if(MALLOC_HOOK_ENABLE_RESOURCE_TRACKING AND
   MALLOC_HOOK_TARGET_OS STREQUAL "linux")
  include(CheckIncludeFileCXX)
  check_include_file_cxx("linux/dma-heap.h" MALLOC_HOOK_HAVE_DMA_HEAP_UAPI)
  if(NOT MALLOC_HOOK_HAVE_DMA_HEAP_UAPI)
    message(FATAL_ERROR
      "Resource tracking requires linux/dma-heap.h; disable "
      "MALLOC_HOOK_ENABLE_RESOURCE_TRACKING for generic Linux builds")
  endif()
endif()

if(ANDROID_ABI)
  set(_malloc_hook_target_processor "${ANDROID_ABI}")
elseif(OHOS_ARCH)
  set(_malloc_hook_target_processor "${OHOS_ARCH}")
else()
  set(_malloc_hook_target_processor "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_malloc_hook_target_processor}" _malloc_hook_target_processor)

if(_malloc_hook_target_processor MATCHES "^(aarch64|arm64|arm64-v8a)$")
  set(MALLOC_HOOK_TARGET_ARCH arm64)
elseif(_malloc_hook_target_processor MATCHES "^(arm|armv7.*|armeabi-v7a)$")
  set(MALLOC_HOOK_TARGET_ARCH arm)
elseif(_malloc_hook_target_processor MATCHES "^(x86_64|amd64)$")
  set(MALLOC_HOOK_TARGET_ARCH x86_64)
elseif(_malloc_hook_target_processor MATCHES "^(x86|i[3-6]86)$")
  set(MALLOC_HOOK_TARGET_ARCH x86)
elseif(_malloc_hook_target_processor STREQUAL "riscv64")
  set(MALLOC_HOOK_TARGET_ARCH riscv64)
else()
  message(FATAL_ERROR
    "Unsupported target architecture '${_malloc_hook_target_processor}'")
endif()

# Fast capture is a compiler-runtime capability, not an OS/libc assumption.
check_cxx_source_compiles([=[
  #include <unwind.h>
  static _Unwind_Reason_Code Probe(struct _Unwind_Context*, void*) {
    return _URC_END_OF_STACK;
  }
  int main() {
    return _Unwind_Backtrace(Probe, nullptr) == _URC_NO_REASON ? 0 : 0;
  }
]=] MALLOC_HOOK_HAVE_COMPILER_UNWIND)

set(MALLOC_HOOK_TARGET_OS "${MALLOC_HOOK_TARGET_OS}" CACHE INTERNAL
    "malloc_hook target OS identity" FORCE)
set(MALLOC_HOOK_TARGET_LIBC "${MALLOC_HOOK_TARGET_LIBC}" CACHE INTERNAL
    "malloc_hook target libc identity" FORCE)
set(MALLOC_HOOK_TARGET_ARCH "${MALLOC_HOOK_TARGET_ARCH}" CACHE INTERNAL
    "malloc_hook target CPU architecture" FORCE)
set(MALLOC_HOOK_HAVE_COMPILER_UNWIND "${MALLOC_HOOK_HAVE_COMPILER_UNWIND}"
    CACHE INTERNAL "_Unwind_Backtrace compiler-runtime capability" FORCE)

add_library(malloc_hook_platform_contract INTERFACE)
string(TOUPPER "${MALLOC_HOOK_TARGET_OS}" _malloc_hook_os_define)
string(TOUPPER "${MALLOC_HOOK_TARGET_LIBC}" _malloc_hook_libc_define)
string(TOUPPER "${MALLOC_HOOK_TARGET_ARCH}" _malloc_hook_arch_define)
if(MALLOC_HOOK_HAVE_COMPILER_UNWIND)
  set(_malloc_hook_compiler_unwind 1)
else()
  set(_malloc_hook_compiler_unwind 0)
endif()
target_compile_definitions(malloc_hook_platform_contract INTERFACE
  "MALLOC_HOOK_TARGET_OS_${_malloc_hook_os_define}=1"
  "MALLOC_HOOK_TARGET_LIBC_${_malloc_hook_libc_define}=1"
  "MALLOC_HOOK_TARGET_ARCH_${_malloc_hook_arch_define}=1"
  "MALLOC_HOOK_HAVE_COMPILER_UNWIND=${_malloc_hook_compiler_unwind}")

if(MALLOC_HOOK_TARGET_OS STREQUAL "android")
  set(MALLOC_HOOK_EXPORT_POLICY android)
  set(MALLOC_HOOK_EXPORT_MMAP_HOOK 1)
  set(MALLOC_HOOK_EXPORT_RESOURCE_HOOKS 1)
  set(MALLOC_HOOK_EXPORT_CPP_NEW_HOOK 0)
  set(VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/version_script.ld)
elseif(MALLOC_HOOK_TARGET_OS STREQUAL "linux")
  set(MALLOC_HOOK_EXPORT_POLICY linux)
  set(MALLOC_HOOK_EXPORT_MMAP_HOOK 1)
  set(MALLOC_HOOK_EXPORT_RESOURCE_HOOKS 1)
  set(MALLOC_HOOK_EXPORT_CPP_NEW_HOOK 0)
  set(VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/version_script.ld)
elseif(MALLOC_HOOK_TARGET_OS STREQUAL "ohos")
  set(MALLOC_HOOK_EXPORT_POLICY ohos)
  set(MALLOC_HOOK_EXPORT_RESOURCE_HOOKS 1)
  set(MALLOC_HOOK_EXPORT_CPP_NEW_HOOK 1)
  if(MALLOC_HOOK_OHOS_MMAP_HOOK)
    set(MALLOC_HOOK_EXPORT_MMAP_HOOK 1)
    set(VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/version_script_ohos_mmap.ld)
  else()
    set(MALLOC_HOOK_EXPORT_MMAP_HOOK 0)
    set(VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/version_script_ohos.ld)
  endif()
else()
  message(FATAL_ERROR
    "No hook export policy for target OS '${MALLOC_HOOK_TARGET_OS}'")
endif()

if(NOT MALLOC_HOOK_ENABLE_RESOURCE_TRACKING)
  set(MALLOC_HOOK_EXPORT_RESOURCE_HOOKS 0)
endif()

# A host smoke build may intentionally disable vendor resource interception.
# Keep the ABI manifest valid in that mode instead of asking the linker to
# export hooks that are not compiled into the library.
if(NOT MALLOC_HOOK_EXPORT_RESOURCE_HOOKS)
  file(READ "${VERSION_SCRIPT}" _malloc_hook_version_script)
  string(REGEX REPLACE
    "[ \t]*(ioctl|close)[ \t]*;[ \t]*\n"
    ""
    _malloc_hook_version_script
    "${_malloc_hook_version_script}")
  set(_malloc_hook_no_resource_script
      "${CMAKE_BINARY_DIR}/malloc_hook_version_script_no_resource.ld")
  file(GENERATE OUTPUT "${_malloc_hook_no_resource_script}"
       CONTENT "${_malloc_hook_version_script}")
  set(VERSION_SCRIPT "${_malloc_hook_no_resource_script}")
endif()

# ARCH-01 capability contract. OS, libc, and CPU identity remain independent
# from capture and hook-source capabilities. These values are exported below
# through the platform contract target so backend and hook code can make
# explicit decisions without using musl/Bionic shortcuts.
set(MALLOC_HOOK_CAP_NATIVE_STACK 1)
set(MALLOC_HOOK_CAP_MANAGED_RUNTIME 0)
set(MALLOC_HOOK_CAP_OTHER_THREAD_CAPTURE 0)
set(MALLOC_HOOK_CAP_OFFLINE_UNWIND 0)
set(MALLOC_HOOK_CAP_DIRECT_SYSCALL_INTERPOSITION 0)
if(MALLOC_HOOK_HAVE_COMPILER_UNWIND OR MALLOC_HOOK_TARGET_OS STREQUAL "linux")
  set(MALLOC_HOOK_CAP_FAST_CAPTURE 1)
else()
  set(MALLOC_HOOK_CAP_FAST_CAPTURE 0)
endif()
if(MALLOC_HOOK_TARGET_OS STREQUAL "android" OR
   MALLOC_HOOK_TARGET_OS STREQUAL "linux" OR
   (MALLOC_HOOK_TARGET_OS STREQUAL "ohos" AND MALLOC_HOOK_HAVE_COMPILER_UNWIND))
  set(MALLOC_HOOK_CAP_ACCURATE_CAPTURE 1)
else()
  set(MALLOC_HOOK_CAP_ACCURATE_CAPTURE 0)
endif()
set(MALLOC_HOOK_CAP_MALLOC_HOOKS 1)
set(MALLOC_HOOK_CAP_CPP_NEW_HOOKS "${MALLOC_HOOK_EXPORT_CPP_NEW_HOOK}")
set(MALLOC_HOOK_CAP_ANONYMOUS_MMAP "${MALLOC_HOOK_EXPORT_MMAP_HOOK}")
set(MALLOC_HOOK_CAP_RESOURCE_IOCTL "${MALLOC_HOOK_EXPORT_RESOURCE_HOOKS}")
set(MALLOC_HOOK_CAP_MREMAP "${MALLOC_HOOK_EXPORT_MMAP_HOOK}")

foreach(_malloc_hook_capability IN ITEMS
    NATIVE_STACK MANAGED_RUNTIME OTHER_THREAD_CAPTURE OFFLINE_UNWIND
    DIRECT_SYSCALL_INTERPOSITION FAST_CAPTURE ACCURATE_CAPTURE
    MALLOC_HOOKS CPP_NEW_HOOKS ANONYMOUS_MMAP RESOURCE_IOCTL MREMAP)
  set(MALLOC_HOOK_CAP_${_malloc_hook_capability}
      "${MALLOC_HOOK_CAP_${_malloc_hook_capability}}" CACHE INTERNAL
      "ARCH-01 capability ${_malloc_hook_capability}" FORCE)
endforeach()

target_compile_definitions(malloc_hook_platform_contract INTERFACE
  "MALLOC_HOOK_CAP_NATIVE_STACK=${MALLOC_HOOK_CAP_NATIVE_STACK}"
  "MALLOC_HOOK_CAP_MANAGED_RUNTIME=${MALLOC_HOOK_CAP_MANAGED_RUNTIME}"
  "MALLOC_HOOK_CAP_OTHER_THREAD_CAPTURE=${MALLOC_HOOK_CAP_OTHER_THREAD_CAPTURE}"
  "MALLOC_HOOK_CAP_OFFLINE_UNWIND=${MALLOC_HOOK_CAP_OFFLINE_UNWIND}"
  "MALLOC_HOOK_CAP_DIRECT_SYSCALL_INTERPOSITION=${MALLOC_HOOK_CAP_DIRECT_SYSCALL_INTERPOSITION}"
  "MALLOC_HOOK_CAP_FAST_CAPTURE=${MALLOC_HOOK_CAP_FAST_CAPTURE}"
  "MALLOC_HOOK_CAP_ACCURATE_CAPTURE=${MALLOC_HOOK_CAP_ACCURATE_CAPTURE}"
  "MALLOC_HOOK_CAP_MALLOC_HOOKS=${MALLOC_HOOK_CAP_MALLOC_HOOKS}"
  "MALLOC_HOOK_CAP_CPP_NEW_HOOKS=${MALLOC_HOOK_CAP_CPP_NEW_HOOKS}"
  "MALLOC_HOOK_CAP_ANONYMOUS_MMAP=${MALLOC_HOOK_CAP_ANONYMOUS_MMAP}"
  "MALLOC_HOOK_CAP_RESOURCE_IOCTL=${MALLOC_HOOK_CAP_RESOURCE_IOCTL}"
  "MALLOC_HOOK_CAP_MREMAP=${MALLOC_HOOK_CAP_MREMAP}")

message(STATUS
  "malloc_hook target contract: os=${MALLOC_HOOK_TARGET_OS}, "
  "libc=${MALLOC_HOOK_TARGET_LIBC}, arch=${MALLOC_HOOK_TARGET_ARCH}, "
  "compiler_unwind=${MALLOC_HOOK_HAVE_COMPILER_UNWIND}, "
  "fast=${MALLOC_HOOK_CAP_FAST_CAPTURE}, "
  "accurate=${MALLOC_HOOK_CAP_ACCURATE_CAPTURE}")

set(MALLOC_HOOK_EXPORT_POLICY "${MALLOC_HOOK_EXPORT_POLICY}" CACHE INTERNAL
    "Selected malloc_hook exported ABI policy" FORCE)
set(MALLOC_HOOK_EXPORT_MMAP_HOOK "${MALLOC_HOOK_EXPORT_MMAP_HOOK}" CACHE INTERNAL
    "Whether the platform ABI exports mmap hooks" FORCE)
set(MALLOC_HOOK_EXPORT_RESOURCE_HOOKS "${MALLOC_HOOK_EXPORT_RESOURCE_HOOKS}"
    CACHE INTERNAL "Whether the platform ABI exports ioctl/close hooks" FORCE)
set(MALLOC_HOOK_EXPORT_CPP_NEW_HOOK "${MALLOC_HOOK_EXPORT_CPP_NEW_HOOK}"
    CACHE INTERNAL "Whether the platform ABI exports C++ new/delete hooks" FORCE)

message(STATUS
  "malloc_hook export policy: name=${MALLOC_HOOK_EXPORT_POLICY}, "
  "mmap=${MALLOC_HOOK_EXPORT_MMAP_HOOK}, "
  "resource=${MALLOC_HOOK_EXPORT_RESOURCE_HOOKS}, "
  "resource_tracking=${MALLOC_HOOK_ENABLE_RESOURCE_TRACKING}, "
  "cpp_new=${MALLOC_HOOK_EXPORT_CPP_NEW_HOOK}")

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
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(MALLOC_HOOK_HAVE_UNWINDSTACK OFF)
if(MALLOC_HOOK_TARGET_OS STREQUAL "android")
  add_subdirectory(unwindstack/cmake)
  set(MALLOC_HOOK_HAVE_UNWINDSTACK ON)
endif()
add_subdirectory(backtrace)

# 添加 .so
add_library(alloc_hook SHARED ${CMAKE_SOURCE_DIR}/src/alloc_hook.cpp)
# 链接需要的库和头文件
target_link_libraries(alloc_hook PRIVATE helper)
target_compile_definitions(alloc_hook PRIVATE
  "MALLOC_HOOK_EXPORT_MMAP_HOOK=${MALLOC_HOOK_EXPORT_MMAP_HOOK}"
  "MALLOC_HOOK_EXPORT_RESOURCE_HOOKS=${MALLOC_HOOK_EXPORT_RESOURCE_HOOKS}"
  "MALLOC_HOOK_EXPORT_CPP_NEW_HOOK=${MALLOC_HOOK_EXPORT_CPP_NEW_HOOK}"
  "MALLOC_HOOK_CAP_MREMAP=${MALLOC_HOOK_CAP_MREMAP}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_link_options(alloc_hook PRIVATE
    "LINKER:--version-script=${VERSION_SCRIPT}")
  set_property(TARGET alloc_hook APPEND PROPERTY LINK_DEPENDS "${VERSION_SCRIPT}")
else()
  message(FATAL_ERROR
    "malloc_hook export hiding requires a Clang or GCC compatible ELF linker")
endif()
# 安装到 out/lib 目录
install(TARGETS alloc_hook DESTINATION out/lib)

if(BUILD_TESTING)
  add_test(
    NAME platform_export_policy
    COMMAND ${CMAKE_COMMAND}
      "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
      "-DTEST_BINARY_DIR=${CMAKE_BINARY_DIR}/platform_export_policy"
      -P ${CMAKE_SOURCE_DIR}/test/platform_export_policy_test.cmake)
  add_test(
    NAME built_platform_export_policy
    COMMAND ${CMAKE_COMMAND}
      "-DLIBRARY=$<TARGET_FILE:alloc_hook>"
      "-DEXPECT_MMAP=${MALLOC_HOOK_EXPORT_MMAP_HOOK}"
      "-DEXPECT_RESOURCE=${MALLOC_HOOK_EXPORT_RESOURCE_HOOKS}"
      "-DEXPECT_CPP=${MALLOC_HOOK_EXPORT_CPP_NEW_HOOK}"
      -P ${CMAKE_SOURCE_DIR}/test/built_export_policy_test.cmake)
  add_test(
    NAME fast_capture_boundary_compile
    COMMAND ${CMAKE_COMMAND}
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
      "-DTEST_BINARY_DIR=${CMAKE_BINARY_DIR}/fast_capture_boundary"
      -P ${CMAKE_SOURCE_DIR}/test/fast_capture_boundary_test.cmake)
  add_test(
    NAME unwindstack_ownership_policy
    COMMAND ${CMAKE_COMMAND}
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
      "-DTEST_BINARY_DIR=${CMAKE_BINARY_DIR}/unwindstack_ownership_policy"
      -P ${CMAKE_SOURCE_DIR}/test/unwindstack_ownership_policy_test.cmake)
  if(MALLOC_HOOK_TARGET_OS STREQUAL "android" AND TARGET unwindstack)
    add_test(
      NAME accurate_unwindstack_build
      COMMAND ${CMAKE_COMMAND}
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        -P ${CMAKE_SOURCE_DIR}/test/accurate_unwindstack_build_test.cmake)
  endif()
  add_test(
    NAME platform_capture_compile
    COMMAND ${CMAKE_COMMAND}
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
      "-DTEST_BINARY_DIR=${CMAKE_BINARY_DIR}/platform_capture_compile"
      -P ${CMAKE_SOURCE_DIR}/test/platform_capture_compile_test.cmake)
  add_test(
    NAME linux_unwind_policy
    COMMAND ${CMAKE_COMMAND}
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
      "-DTEST_BINARY_DIR=${CMAKE_BINARY_DIR}/linux_unwind_policy"
      -P ${CMAKE_SOURCE_DIR}/test/linux_unwind_policy_test.cmake)
  add_test(
    NAME hook_source_boundary
    COMMAND ${CMAKE_COMMAND}
      "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
      -P ${CMAKE_SOURCE_DIR}/test/hook_source_boundary_test.cmake)
  add_test(
    NAME platform_gate_manifest
    COMMAND ${CMAKE_COMMAND}
      "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
      -P ${CMAKE_SOURCE_DIR}/test/platform_gate_manifest_test.cmake)
  find_program(MALLOC_HOOK_GXX NAMES g++)
  if(MALLOC_HOOK_GXX)
    add_test(
      NAME linux_gcc_build
      COMMAND ${CMAKE_COMMAND}
        "-DCXX_COMPILER=${MALLOC_HOOK_GXX}"
        "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}/linux-gcc"
        -P ${CMAKE_SOURCE_DIR}/test/cross_platform_build_test.cmake)
  endif()
  find_program(MALLOC_HOOK_CLANGXX NAMES clang++)
  if(MALLOC_HOOK_CLANGXX)
    add_test(
      NAME linux_clang_build
      COMMAND ${CMAKE_COMMAND}
        "-DCXX_COMPILER=${MALLOC_HOOK_CLANGXX}"
        "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}/linux-clang"
        -P ${CMAKE_SOURCE_DIR}/test/cross_platform_build_test.cmake)
  endif()
  if(MALLOC_HOOK_ANDROID_TOOLCHAIN_FILE)
    add_test(
      NAME android_arm64_build
      COMMAND ${CMAKE_COMMAND}
        "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}/android-arm64"
        "-DTOOLCHAIN_FILE=${MALLOC_HOOK_ANDROID_TOOLCHAIN_FILE}"
        "-DANDROID_ABI=arm64-v8a"
        "-DANDROID_PLATFORM=android-21"
        -P ${CMAKE_SOURCE_DIR}/test/toolchain_build_gate_test.cmake)
  endif()
  if(MALLOC_HOOK_OHOS_TOOLCHAIN_FILE)
    add_test(
      NAME ohos_arm64_build
      COMMAND ${CMAKE_COMMAND}
        "-DREPO_ROOT=${CMAKE_SOURCE_DIR}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}/ohos-arm64"
        "-DTOOLCHAIN_FILE=${MALLOC_HOOK_OHOS_TOOLCHAIN_FILE}"
        "-DOHOS_ARCH=arm64-v8a"
        -P ${CMAKE_SOURCE_DIR}/test/toolchain_build_gate_test.cmake)
  endif()
endif()

if (MALLOC_HOOK_BUILD_TESTS)
  set(MALLOC_HOOK_HAVE_GTEST OFF)
  if (TARGET gtest)
    set(MALLOC_HOOK_HAVE_GTEST ON)
  elseif(EXISTS "${PROJECT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
    add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/googletest)
    set(MALLOC_HOOK_HAVE_GTEST ON)
  else()
    message(WARNING
      "Vendored googletest is unavailable; building dependency-free smoke gates only")
  endif()
  if (EXISTS "${PROJECT_SOURCE_DIR}/third_party/opencl_stub/CMakeLists.txt")
    add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/opencl_stub)
  endif()
  add_subdirectory(test)
endif()

# copy database compile_commands.json to PROJECT_SOURCE_DIR
if(EXISTS "${PROJECT_BINARY_DIR}/compile_commands.json")
  add_custom_target(
    copy_database_compile_commands_mem_trace ALL
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_BINARY_DIR}/compile_commands.json
      ${PROJECT_SOURCE_DIR}/compile_commands.json)
endif()
