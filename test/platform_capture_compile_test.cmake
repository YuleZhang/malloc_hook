cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED CXX_COMPILER OR NOT DEFINED REPO_ROOT OR NOT DEFINED TEST_BINARY_DIR)
  message(FATAL_ERROR "CXX_COMPILER, REPO_ROOT, and TEST_BINARY_DIR are required")
endif()

file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")
set(SOURCE "${REPO_ROOT}/backtrace/src/UnwindBacktrace.cpp")
set(INCLUDES
    "-I${REPO_ROOT}/backtrace/include"
    "-I${REPO_ROOT}/unwindstack"
    "-I${REPO_ROOT}/unwindstack/include")

foreach(PLATFORM IN ITEMS ANDROID OHOS LINUX)
  set(DEFINES
      "-DMALLOC_HOOK_TARGET_OS_${PLATFORM}=1"
      "-DMALLOC_HOOK_HAVE_COMPILER_UNWIND=1")
  if(PLATFORM STREQUAL "ANDROID")
    list(APPEND DEFINES "-DMALLOC_HOOK_ENABLE_LEGACY_UNWINDSTACK_ADAPTER=1")
  endif()
  execute_process(
    COMMAND "${CXX_COMPILER}" -std=c++17 -fsyntax-only ${DEFINES} ${INCLUDES} "${SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR
      "${PLATFORM} capture backend compile failed:\n${stdout}\n${stderr}")
  endif()
endforeach()

set(OHOS_PROBE "${TEST_BINARY_DIR}/ohos_capture_fallback_probe")
execute_process(
  COMMAND "${CXX_COMPILER}" -std=c++17 -pthread
      "-DMALLOC_HOOK_TARGET_OS_OHOS=1"
      "-DMALLOC_HOOK_HAVE_COMPILER_UNWIND=1"
      "-I${REPO_ROOT}/backtrace/include"
      "${SOURCE}" "${REPO_ROOT}/test/ohos_capture_fallback_probe.cpp"
      -o "${OHOS_PROBE}"
  RESULT_VARIABLE ohos_probe_compile_result
  OUTPUT_VARIABLE ohos_probe_compile_stdout
  ERROR_VARIABLE ohos_probe_compile_stderr)
if(NOT ohos_probe_compile_result EQUAL 0)
  message(FATAL_ERROR
    "OHOS Accurate fallback probe failed to compile:\n"
    "${ohos_probe_compile_stdout}\n${ohos_probe_compile_stderr}")
endif()
execute_process(
  COMMAND "${OHOS_PROBE}"
  RESULT_VARIABLE ohos_probe_run_result
  OUTPUT_VARIABLE ohos_probe_run_stdout
  ERROR_VARIABLE ohos_probe_run_stderr)
if(NOT ohos_probe_run_result EQUAL 0)
  message(FATAL_ERROR
    "OHOS Accurate fallback identity probe failed:\n"
    "${ohos_probe_run_stdout}\n${ohos_probe_run_stderr}")
endif()

message(STATUS "Android/Linux/OHOS capture backend compile gates passed")
