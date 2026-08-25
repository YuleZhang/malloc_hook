cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED CXX_COMPILER OR NOT DEFINED REPO_ROOT OR NOT DEFINED TEST_BINARY_DIR)
  message(FATAL_ERROR "CXX_COMPILER, REPO_ROOT, and TEST_BINARY_DIR are required")
endif()

file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")
set(PROBE "${TEST_BINARY_DIR}/pointer_stack_boundary_probe.cpp")
file(WRITE "${PROBE}" "#include \"PointerData.h\"\nint main() { return 0; }\n")

execute_process(
  COMMAND "${CXX_COMPILER}" -std=c++17 -E -P
          "-I${REPO_ROOT}/backtrace/include" "${PROBE}"
  RESULT_VARIABLE preprocess_result
  OUTPUT_VARIABLE preprocessed
  ERROR_VARIABLE preprocess_stderr)
if(NOT preprocess_result EQUAL 0)
  message(FATAL_ERROR
    "Pointer stack boundary preprocessing failed:\n${preprocess_stderr}")
endif()

foreach(FORBIDDEN IN ITEMS unwindstack Dl_info)
  string(FIND "${preprocessed}" "${FORBIDDEN}" forbidden_position)
  if(NOT forbidden_position EQUAL -1)
    message(FATAL_ERROR
      "Pointer/report boundary exposed backend type '${FORBIDDEN}'")
  endif()
endforeach()

message(STATUS "Pointer/report stack boundary is backend-neutral")
