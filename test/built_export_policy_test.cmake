cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED LIBRARY OR NOT DEFINED EXPECT_MMAP OR
   NOT DEFINED EXPECT_RESOURCE OR NOT DEFINED EXPECT_CPP)
  message(FATAL_ERROR
    "LIBRARY, EXPECT_MMAP, EXPECT_RESOURCE, and EXPECT_CPP are required")
endif()
if(NOT EXISTS "${LIBRARY}")
  message(FATAL_ERROR "Built hook library does not exist: ${LIBRARY}")
endif()

find_program(NM_TOOL NAMES llvm-nm nm REQUIRED)
execute_process(
  COMMAND "${NM_TOOL}" -D --defined-only --format=posix "${LIBRARY}"
  RESULT_VARIABLE NM_RESULT
  OUTPUT_VARIABLE NM_OUTPUT
  ERROR_VARIABLE NM_ERROR)
if(NOT NM_RESULT EQUAL 0)
  message(FATAL_ERROR "Built-library symbol inspection failed: ${NM_ERROR}")
endif()

set(ACTUAL_EXPORTS)
string(REPLACE "\n" ";" NM_LINES "${NM_OUTPUT}")
foreach(LINE IN LISTS NM_LINES)
  if(LINE MATCHES "^([^ ]+) ")
    list(APPEND ACTUAL_EXPORTS "${CMAKE_MATCH_1}")
  endif()
endforeach()
list(SORT ACTUAL_EXPORTS)

set(EXPECTED_EXPORTS
    aligned_alloc
    calloc
    checkpoint
    free
    malloc
    memalign
    posix_memalign
    realloc)
if(EXPECT_RESOURCE)
  list(APPEND EXPECTED_EXPORTS ioctl close)
endif()
if(EXPECT_MMAP)
  list(APPEND EXPECTED_EXPORTS mmap mmap64 mremap munmap)
endif()
if(EXPECT_CPP)
  list(APPEND EXPECTED_EXPORTS
      _ZdaPv _ZdaPvRKSt9nothrow_t _ZdaPvj _ZdaPvjSt11align_val_t
      _ZdaPvm _ZdaPvmSt11align_val_t _ZdaPvSt11align_val_t
      _ZdaPvSt11align_val_tRKSt9nothrow_t _ZdlPv _ZdlPvRKSt9nothrow_t
      _ZdlPvj _ZdlPvjSt11align_val_t _ZdlPvm _ZdlPvmSt11align_val_t
      _ZdlPvSt11align_val_t _ZdlPvSt11align_val_tRKSt9nothrow_t
      _Znam _ZnamRKSt9nothrow_t _ZnamSt11align_val_t
      _ZnamSt11align_val_tRKSt9nothrow_t _Znaj _ZnajRKSt9nothrow_t
      _ZnajSt11align_val_t _ZnajSt11align_val_tRKSt9nothrow_t
      _Znwm _ZnwmRKSt9nothrow_t _ZnwmSt11align_val_t
      _ZnwmSt11align_val_tRKSt9nothrow_t _Znwj _ZnwjRKSt9nothrow_t
      _ZnwjSt11align_val_t _ZnwjSt11align_val_tRKSt9nothrow_t)
endif()
list(SORT EXPECTED_EXPORTS)

if(NOT ACTUAL_EXPORTS STREQUAL EXPECTED_EXPORTS)
  message(FATAL_ERROR
    "Built-library export mismatch\nexpected: ${EXPECTED_EXPORTS}\n"
    "actual: ${ACTUAL_EXPORTS}")
endif()

foreach(PRIVATE_SYMBOL IN ITEMS
    helper_private
    _ZN11unwindstack12HiddenSymbolEv
    _ZN7android4base12HiddenSymbolEv
    backtrace_private)
  if(PRIVATE_SYMBOL IN_LIST ACTUAL_EXPORTS)
    message(FATAL_ERROR "Built hook library leaked ${PRIVATE_SYMBOL}")
  endif()
endforeach()

message(STATUS "Verified built hook exports: ${LIBRARY}")
