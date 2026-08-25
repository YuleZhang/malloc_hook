if(NOT DEFINED CXX_COMPILER OR NOT DEFINED REPO_ROOT OR NOT DEFINED TEST_BINARY_DIR)
  message(FATAL_ERROR "CXX_COMPILER, REPO_ROOT, and TEST_BINARY_DIR are required")
endif()

file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")
set(source "${REPO_ROOT}/test/fast_capture_boundary_probe.cpp")
execute_process(
  COMMAND "${CXX_COMPILER}" -std=c++17 -fsyntax-only
          "-I${REPO_ROOT}/backtrace/include"
          "${source}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
  message(FATAL_ERROR
    "Fast capture boundary must compile without unwindstack headers:\n${stdout}\n${stderr}")
endif()

execute_process(
  COMMAND "${CXX_COMPILER}" -std=c++17 -E -P
          "-I${REPO_ROOT}/backtrace/include"
          "${source}"
  RESULT_VARIABLE preprocess_result
  OUTPUT_VARIABLE preprocessed
  ERROR_VARIABLE preprocess_stderr)
if(NOT preprocess_result EQUAL 0)
  message(FATAL_ERROR "Fast capture boundary preprocessing failed:\n${preprocess_stderr}")
endif()
string(FIND "${preprocessed}" "unwindstack" unwindstack_position)
if(NOT unwindstack_position EQUAL -1)
  message(FATAL_ERROR
    "Fast capture boundary exposed an unwindstack domain name")
endif()
