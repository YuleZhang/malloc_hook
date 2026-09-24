cmake_minimum_required(VERSION 3.23)

# Package version: the VERSION file is the source of truth (tag as v<VERSION>);
# override with -DMALLOC_HOOK_VERSION=... for a one-off build.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION _MALLOC_HOOK_VERSION_FILE)
set(MALLOC_HOOK_VERSION "${_MALLOC_HOOK_VERSION_FILE}" CACHE STRING "malloc_hook package version")

project(malloc_hook VERSION ${MALLOC_HOOK_VERSION} LANGUAGES C CXX ASM)

option(MALLOC_HOOK_BUILD_TESTS "Build malloc_hook test binaries" ON)
# mmap/munmap/mremap interposition is a platform-neutral capability, but its
# default differs: OHOS leaves it off to limit loader/vendor runtime
# interference, while Android and glibc Linux export it by policy. The target
# identity (ANDROID/OHOS) is set by the toolchain before options are declared,
# so the default can be selected here without a separate per-OS switch.
if(OHOS)
  set(_malloc_hook_mmap_hook_default OFF)
else()
  set(_malloc_hook_mmap_hook_default ON)
endif()
option(ENABLE_MMAP_HOOK_EXPORT
  "Export mmap/munmap/mremap hooks (default OFF on OHOS, ON elsewhere)"
  ${_malloc_hook_mmap_hook_default})
# DMA/ION/GPU buffer capture is part of what this tool measures, not an add-on:
# on every supported target the majority of a camera/NN pipeline's memory is
# DMA-BUF, so it is on by default. Turn it off only for host smoke builds that
# have no driver UAPI at all.
option(MALLOC_HOOK_ENABLE_DMA_CAPTURE
  "Capture DMA-BUF/ION/GPU buffer allocations in addition to malloc/mmap" ON)
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

if(NOT MALLOC_HOOK_ENABLE_DMA_CAPTURE)
  set(MALLOC_HOOK_EXPORT_RESOURCE_HOOKS 0)
endif()

if(MALLOC_HOOK_ENABLE_DMA_CAPTURE AND
   MALLOC_HOOK_TARGET_OS STREQUAL "linux")
  include(CheckIncludeFileCXX)
  check_include_file_cxx("linux/dma-heap.h" MALLOC_HOOK_HAVE_DMA_HEAP_UAPI)
  if(NOT MALLOC_HOOK_HAVE_DMA_HEAP_UAPI)
    # Not fatal: a vendored copy of the dma-heap UAPI ships with the driver
    # headers, because cross toolchains routinely omit it from the sysroot even
    # when the target kernel exports /dev/dma_heap.
    message(STATUS
      "linux/dma-heap.h not in sysroot; using vendored dma-heap UAPI")
  endif()
endif()
if(MALLOC_HOOK_HAVE_DMA_HEAP_UAPI)
  set(_malloc_hook_dma_heap_uapi 1)
else()
  set(_malloc_hook_dma_heap_uapi 0)
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
  "MALLOC_HOOK_HAVE_DMA_HEAP_UAPI=${_malloc_hook_dma_heap_uapi}"
  "MALLOC_HOOK_HAVE_COMPILER_UNWIND=${_malloc_hook_compiler_unwind}")

# mmap export is a single capability across every target now; only its default
# differs by OS (selected above). The version script for each policy lists the
# mmap family, and the strip pass below removes it when the capability is off.
if(ENABLE_MMAP_HOOK_EXPORT)
  set(MALLOC_HOOK_EXPORT_MMAP_HOOK 1)
else()
  set(MALLOC_HOOK_EXPORT_MMAP_HOOK 0)
endif()

if(MALLOC_HOOK_TARGET_OS STREQUAL "android")
  set(MALLOC_HOOK_EXPORT_POLICY android)
  set(MALLOC_HOOK_EXPORT_RESOURCE_HOOKS 1)
  set(MALLOC_HOOK_EXPORT_CPP_NEW_HOOK 0)
  set(VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/version_script.ld)
elseif(MALLOC_HOOK_TARGET_OS STREQUAL "linux")
  set(MALLOC_HOOK_EXPORT_POLICY linux)
  set(MALLOC_HOOK_EXPORT_RESOURCE_HOOKS 1)
  set(MALLOC_HOOK_EXPORT_CPP_NEW_HOOK 0)
  set(VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/version_script.ld)
elseif(MALLOC_HOOK_TARGET_OS STREQUAL "ohos")
  set(MALLOC_HOOK_EXPORT_POLICY ohos)
  set(MALLOC_HOOK_EXPORT_RESOURCE_HOOKS 1)
  set(MALLOC_HOOK_EXPORT_CPP_NEW_HOOK 1)
  set(VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/version_script_ohos.ld)
else()
  message(FATAL_ERROR
    "No hook export policy for target OS '${MALLOC_HOOK_TARGET_OS}'")
endif()

if(NOT MALLOC_HOOK_ENABLE_DMA_CAPTURE)
  set(MALLOC_HOOK_EXPORT_RESOURCE_HOOKS 0)
endif()

# The selected version script lists every hook this project can export. Symbols
# for a capability that is off in this build are stripped here so the linker is
# never asked to export a hook that was not compiled in: ioctl/close when
# resource tracking is off (host smoke builds), and the mmap family when mmap
# export is off (OHOS default, or an explicit opt-out on any platform).
set(_malloc_hook_strip_symbols "")
if(NOT MALLOC_HOOK_EXPORT_RESOURCE_HOOKS)
  list(APPEND _malloc_hook_strip_symbols "ioctl" "close")
endif()
if(NOT MALLOC_HOOK_EXPORT_MMAP_HOOK)
  list(APPEND _malloc_hook_strip_symbols "mmap" "mmap64" "munmap" "mremap")
endif()
if(_malloc_hook_strip_symbols)
  file(READ "${VERSION_SCRIPT}" _malloc_hook_version_script)
  string(REPLACE ";" "|" _malloc_hook_strip_alternation "${_malloc_hook_strip_symbols}")
  string(REGEX REPLACE
    "[ \t]*(${_malloc_hook_strip_alternation})[ \t]*;[ \t]*\n"
    ""
    _malloc_hook_version_script
    "${_malloc_hook_version_script}")
  set(_malloc_hook_stripped_script
      "${CMAKE_BINARY_DIR}/malloc_hook_version_script_stripped.ld")
  file(GENERATE OUTPUT "${_malloc_hook_stripped_script}"
       CONTENT "${_malloc_hook_version_script}")
  set(VERSION_SCRIPT "${_malloc_hook_stripped_script}")
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
  "resource_tracking=${MALLOC_HOOK_ENABLE_DMA_CAPTURE}, "
  "cpp_new=${MALLOC_HOOK_EXPORT_CPP_NEW_HOOK}")

if(MALLOC_HOOK_BUILD_TESTS AND MALLOC_HOOK_BUILD_GL_TESTS AND ANDROID)
  set(_malloc_hook_build_gl_tests_effective ON)
else()
  set(_malloc_hook_build_gl_tests_effective OFF)
endif()

add_custom_target(print_build_options
  COMMAND ${CMAKE_COMMAND} -E echo "malloc_hook effective build options:"
  COMMAND ${CMAKE_COMMAND} -E echo "  CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
  COMMAND ${CMAKE_COMMAND} -E echo "  compiler=${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
  COMMAND ${CMAKE_COMMAND} -E echo "  target=${MALLOC_HOOK_TARGET_OS}/${MALLOC_HOOK_TARGET_LIBC}/${MALLOC_HOOK_TARGET_ARCH}"
  COMMAND ${CMAKE_COMMAND} -E echo "  MALLOC_HOOK_ENABLE_DMA_CAPTURE=${MALLOC_HOOK_ENABLE_DMA_CAPTURE}"
  COMMAND ${CMAKE_COMMAND} -E echo "  ENABLE_MMAP_HOOK_EXPORT=${ENABLE_MMAP_HOOK_EXPORT}"
  COMMAND ${CMAKE_COMMAND} -E echo "  MALLOC_HOOK_BUILD_TESTS=${MALLOC_HOOK_BUILD_TESTS}"
  COMMAND ${CMAKE_COMMAND} -E echo "  MALLOC_HOOK_BUILD_GL_TESTS=${MALLOC_HOOK_BUILD_GL_TESTS}"
  COMMAND ${CMAKE_COMMAND} -E echo "  build_gl_tests_effective=${_malloc_hook_build_gl_tests_effective}"
  COMMAND ${CMAKE_COMMAND} -E echo "  compiler_unwind=${MALLOC_HOOK_HAVE_COMPILER_UNWIND}"
  COMMAND ${CMAKE_COMMAND} -E echo "  dma_heap_uapi=${_malloc_hook_dma_heap_uapi}"
  COMMAND ${CMAKE_COMMAND} -E echo "  export_policy=${MALLOC_HOOK_EXPORT_POLICY}"
  COMMAND ${CMAKE_COMMAND} -E echo "  export_mmap=${MALLOC_HOOK_EXPORT_MMAP_HOOK}"
  COMMAND ${CMAKE_COMMAND} -E echo "  export_resource=${MALLOC_HOOK_EXPORT_RESOURCE_HOOKS}"
  COMMAND ${CMAKE_COMMAND} -E echo "  export_cpp_new=${MALLOC_HOOK_EXPORT_CPP_NEW_HOOK}"
  COMMENT "Printing effective malloc_hook build options"
  VERBATIM)

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

# --------------------------------------------------------------------------- #
# Distribution package (cpack): a self-contained tarball with the hook, the
# Python trace tooling and the docs, for quick sharing and use.
#   cmake --build <build> && (cd <build> && cpack)
# -> malloc_hook-<version>-<abi>.tar.gz : lib/ + scripts/ + README + VERSION.
# The `dist` component is EXCLUDE_FROM_ALL so a plain `ninja install` (used by
# the platform build_*.sh scripts) is unaffected.
# --------------------------------------------------------------------------- #
install(TARGETS alloc_hook LIBRARY DESTINATION lib COMPONENT dist EXCLUDE_FROM_ALL)
install(FILES
    ${CMAKE_SOURCE_DIR}/scripts/process_memory_stack.py
    ${CMAKE_SOURCE_DIR}/scripts/build_perfetto_alloc_track.py
    ${CMAKE_SOURCE_DIR}/scripts/perfetto_proto.py
    ${CMAKE_SOURCE_DIR}/scripts/merge_csv_to_perfetto.py
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
  set(_MH_ABI "android-${ANDROID_ABI}")
elseif(OHOS)
  set(_MH_ABI "ohos-${CMAKE_SYSTEM_PROCESSOR}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CROSSCOMPILING)
  set(_MH_ABI "linux-${CMAKE_SYSTEM_PROCESSOR}")
else()
  set(_MH_ABI "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(CPACK_ARCHIVE_DIST_FILE_NAME "malloc_hook-${MALLOC_HOOK_VERSION}-${_MH_ABI}")
set(CPACK_PACKAGE_FILE_NAME "malloc_hook-${MALLOC_HOOK_VERSION}-${_MH_ABI}")
include(CPack)
