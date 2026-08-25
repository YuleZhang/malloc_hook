cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED REPO_ROOT OR NOT DEFINED CXX_COMPILER OR NOT DEFINED TEST_BINARY_DIR)
  message(FATAL_ERROR "REPO_ROOT, CXX_COMPILER, and TEST_BINARY_DIR are required")
endif()

file(READ "${REPO_ROOT}/backtrace/src/UnwindBacktrace.cpp" source)
foreach(forbidden IN ITEMS "/proc/self/maps")
  string(FIND "${source}" "${forbidden}" position)
  if(NOT position EQUAL -1)
    message(FATAL_ERROR
      "Linux capture adapter must not contain ${forbidden} policy: ${source}")
  endif()
endforeach()
if(source MATCHES "(^|[^A-Za-z])ART([^A-Za-z]|$)" OR
   source MATCHES "(^|[^A-Za-z])JIT([^A-Za-z]|$)")
  message(FATAL_ERROR "Linux capture adapter must not contain ART/JIT policy")
endif()
string(FIND "${source}" "defined(MALLOC_HOOK_TARGET_OS_LINUX)" linux_guard)
if(linux_guard EQUAL -1)
  message(FATAL_ERROR "Linux capture route is not explicitly guarded")
endif()
string(FIND "${source}" "StackCaptureBackend::LinuxNative" linux_backend)
if(linux_backend EQUAL -1)
  message(FATAL_ERROR "Linux native backend metadata is missing")
endif()

file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")
set(source_file "${REPO_ROOT}/backtrace/src/UnwindBacktrace.cpp")
set(probe "${TEST_BINARY_DIR}/linux_unwind_probe.cpp")
file(WRITE "${probe}" [=[
#include "UnwindBacktrace.h"
#include <cstdlib>
int main() {
  const RawStackRecord fast = CaptureStack(StackCaptureMode::Fast, 8, 0);
  const RawStackRecord accurate = CaptureStack(StackCaptureMode::Accurate, 8, 0);
  if (accurate.backend != StackCaptureBackend::LinuxNative ||
      accurate.frame_count == 0 ||
      accurate.frame_count > 8) {
    return 1;
  }
#if MALLOC_HOOK_HAVE_COMPILER_UNWIND
  if (fast.backend != StackCaptureBackend::CompilerUnwind ||
      fast.frame_count == 0 ||
      fast.frame_count > 8) {
    return 2;
  }
#else
  if (fast.backend != StackCaptureBackend::Fallback ||
      fast.capture_state != StackCaptureState::Error) {
    return 3;
  }
#endif
  return 0;
}
]=])
execute_process(
  COMMAND "${CXX_COMPILER}" -std=c++17 -O0
          "-DMALLOC_HOOK_TARGET_OS_LINUX=1"
          "-DMALLOC_HOOK_HAVE_COMPILER_UNWIND=1"
          "-I${REPO_ROOT}/backtrace/include"
          "${source_file}" "${probe}" -o "${TEST_BINARY_DIR}/linux_unwind_probe"
  RESULT_VARIABLE compile_result
  OUTPUT_VARIABLE compile_stdout
  ERROR_VARIABLE compile_stderr)
if(NOT compile_result EQUAL 0)
  message(FATAL_ERROR
    "Linux capture smoke probe failed to compile:\n${compile_stdout}\n${compile_stderr}")
endif()
execute_process(
  COMMAND "${TEST_BINARY_DIR}/linux_unwind_probe"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR
    "Linux capture smoke probe failed:\n${run_stdout}\n${run_stderr}")
endif()

execute_process(
  COMMAND "${CXX_COMPILER}" -std=c++17 -E -P
          "-DMALLOC_HOOK_TARGET_OS_LINUX=1"
          "-DMALLOC_HOOK_HAVE_COMPILER_UNWIND=1"
          "-I${REPO_ROOT}/backtrace/include"
          "${source_file}"
  RESULT_VARIABLE preprocess_result
  OUTPUT_VARIABLE preprocessed
  ERROR_VARIABLE preprocess_stderr)
if(NOT preprocess_result EQUAL 0)
  message(FATAL_ERROR "Linux capture preprocessing failed:\n${preprocess_stderr}")
endif()
foreach(forbidden IN ITEMS "AndroidUnwinder" "unwindstack::" "MALLOC_HOOK_TARGET_OS_OHOS")
  string(FIND "${preprocessed}" "${forbidden}" position)
  if(NOT position EQUAL -1)
    message(FATAL_ERROR
      "Linux preprocessed capture route contains forbidden ${forbidden} code")
  endif()
endforeach()

message(STATUS "Linux unwind policy and bounded-stack smoke checks passed")
