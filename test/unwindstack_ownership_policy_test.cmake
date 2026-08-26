cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED REPO_ROOT OR NOT DEFINED CXX_COMPILER OR NOT DEFINED TEST_BINARY_DIR)
  message(FATAL_ERROR "REPO_ROOT, CXX_COMPILER, and TEST_BINARY_DIR are required")
endif()

set(metadata "${REPO_ROOT}/unwindstack/UPSTREAM_BASELINE.md")
if(NOT EXISTS "${metadata}")
  message(FATAL_ERROR "Missing durable unwindstack ownership metadata: ${metadata}")
endif()
file(READ "${metadata}" metadata_text)
foreach(section IN ITEMS "## Upstream baseline" "## Local deviations")
  string(FIND "${metadata_text}" "${section}" section_position)
  if(section_position EQUAL -1)
    message(FATAL_ERROR "Ownership metadata is missing '${section}'")
  endif()
endforeach()

file(READ "${REPO_ROOT}/backtrace/CMakeLists.txt" backtrace_cmake)
string(FIND "${backtrace_cmake}"
       "if(MALLOC_HOOK_TARGET_OS STREQUAL \"android\")"
       android_gate)
if(android_gate EQUAL -1)
  message(FATAL_ERROR "unwindstack adapter is not guarded by the Android target gate")
endif()
string(FIND "${backtrace_cmake}"
       "MALLOC_HOOK_ENABLE_LEGACY_UNWINDSTACK_ADAPTER=1"
       adapter_define)
if(adapter_define EQUAL -1)
  message(FATAL_ERROR "legacy unwindstack adapter capability is not explicit")
endif()
string(FIND "${backtrace_cmake}" "target_link_libraries(helper PRIVATE unwindstack)"
       private_link)
if(private_link EQUAL -1)
  message(FATAL_ERROR "unwindstack must remain a private helper dependency")
endif()

file(READ "${REPO_ROOT}/CMakeLists.txt" root_cmake)
string(FIND "${root_cmake}" "add_subdirectory(unwindstack/cmake)" vendor_subdir)
if(vendor_subdir EQUAL -1)
  message(FATAL_ERROR "vendored unwindstack target is not declared")
endif()
string(FIND "${root_cmake}"
       "if(MALLOC_HOOK_TARGET_OS STREQUAL \"android\")"
       root_android_gate)
if(root_android_gate EQUAL -1)
  message(FATAL_ERROR "root build does not expose an Android-specific unwindstack gate")
endif()

file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")
set(probe "${REPO_ROOT}/test/fast_capture_boundary_probe.cpp")
execute_process(
  COMMAND "${CXX_COMPILER}" -std=c++17 -fsyntax-only
          "-I${REPO_ROOT}/backtrace/include" "${probe}"
  RESULT_VARIABLE compile_result
  OUTPUT_VARIABLE compile_stdout
  ERROR_VARIABLE compile_stderr)
if(NOT compile_result EQUAL 0)
  message(FATAL_ERROR
    "Fast capture boundary unexpectedly requires unwindstack:\n"
    "${compile_stdout}\n${compile_stderr}")
endif()

message(STATUS "unwindstack ownership policy checks passed")
