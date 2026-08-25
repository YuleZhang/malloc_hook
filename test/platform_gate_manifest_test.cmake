cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED REPO_ROOT)
  message(FATAL_ERROR "REPO_ROOT is required")
endif()

foreach(required_file IN ITEMS
    test/platform_export_policy_test.cmake
    test/built_export_policy_test.cmake
    test/platform_capture_compile_test.cmake
    test/fast_capture_boundary_test.cmake
    test/linux_unwind_policy_test.cmake
    test/hook_source_boundary_test.cmake
    test/toolchain_build_gate_test.cmake
    test/cross_platform_build_test.cmake
    test/runtime_smoke_gate_test.cpp
    test/async_lifecycle_identity_test.cpp
    test/mremap_lifecycle_test.cpp)
  if(NOT EXISTS "${REPO_ROOT}/${required_file}")
    message(FATAL_ERROR "Missing required platform gate: ${required_file}")
  endif()
endforeach()

file(READ "${REPO_ROOT}/CMakeLists.txt" root_cmake)
foreach(required_registration IN ITEMS
    "NAME platform_export_policy"
    "NAME built_platform_export_policy"
    "NAME fast_capture_boundary_compile"
    "NAME platform_capture_compile"
    "NAME linux_unwind_policy"
    "NAME linux_gcc_build"
    "NAME linux_clang_build"
    "NAME android_arm64_build"
    "NAME ohos_arm64_build")
  string(FIND "${root_cmake}" "${required_registration}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing platform gate registration: ${required_registration}")
  endif()
endforeach()

file(READ "${REPO_ROOT}/test/CMakeLists.txt" test_cmake)
foreach(required_registration IN ITEMS
    "NAME async_lifecycle_identity"
    "NAME runtime_smoke_gate"
    "NAME mremap_lifecycle")
  string(FIND "${test_cmake}" "${required_registration}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing lifecycle/performance gate registration: ${required_registration}")
  endif()
endforeach()
string(FIND "${root_cmake}" "NAME hook_source_boundary" hook_source_position)
if(hook_source_position EQUAL -1)
  message(FATAL_ERROR "Missing lifecycle/performance gate registration: NAME hook_source_boundary")
endif()

message(STATUS
  "Platform gate manifest passed; Android/OHOS toolchain tests remain conditional "
  "on configured toolchain files")
