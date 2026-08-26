cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED REPO_ROOT OR NOT DEFINED TEST_BINARY_DIR)
  message(FATAL_ERROR "REPO_ROOT and TEST_BINARY_DIR are required")
endif()

find_program(NM_TOOL NAMES llvm-nm nm REQUIRED)
find_program(GNU_CXX NAMES g++)
find_program(CLANG_CXX NAMES clang++)

set(COMPILERS)
if(GNU_CXX)
  list(APPEND COMPILERS "${GNU_CXX}")
endif()
if(CLANG_CXX)
  list(APPEND COMPILERS "${CLANG_CXX}")
endif()
list(REMOVE_DUPLICATES COMPILERS)
if(NOT COMPILERS)
  message(FATAL_ERROR "Neither GCC nor Clang is available for export-policy tests")
endif()

set(COMMON_EXPORTS
    aligned_alloc
    calloc
    checkpoint
    close
    free
    ioctl
    malloc
    memalign
    mmap
    mmap64
    mremap
    munmap
    posix_memalign
    realloc)
set(CPP_EXPORTS
    _ZdaPv
    _ZdaPvRKSt9nothrow_t
    _ZdaPvj
    _ZdaPvjSt11align_val_t
    _ZdaPvm
    _ZdaPvmSt11align_val_t
    _ZdaPvSt11align_val_t
    _ZdaPvSt11align_val_tRKSt9nothrow_t
    _ZdlPv
    _ZdlPvRKSt9nothrow_t
    _ZdlPvj
    _ZdlPvjSt11align_val_t
    _ZdlPvm
    _ZdlPvmSt11align_val_t
    _ZdlPvSt11align_val_t
    _ZdlPvSt11align_val_tRKSt9nothrow_t
    _Znam
    _ZnamRKSt9nothrow_t
    _ZnamSt11align_val_t
    _ZnamSt11align_val_tRKSt9nothrow_t
    _Znaj
    _ZnajRKSt9nothrow_t
    _ZnajSt11align_val_t
    _ZnajSt11align_val_tRKSt9nothrow_t
    _Znwm
    _ZnwmRKSt9nothrow_t
    _ZnwmSt11align_val_t
    _ZnwmSt11align_val_tRKSt9nothrow_t
    _Znwj
    _ZnwjRKSt9nothrow_t
    _ZnwjSt11align_val_t
    _ZnwjSt11align_val_tRKSt9nothrow_t)

set(ALL_FIXTURE_SYMBOLS ${COMMON_EXPORTS} ${CPP_EXPORTS}
    helper_private
    _ZN11unwindstack12HiddenSymbolEv
    _ZN7android4base12HiddenSymbolEv
    backtrace_private)
list(REMOVE_DUPLICATES ALL_FIXTURE_SYMBOLS)

file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")
set(FIXTURE_SOURCE "${TEST_BINARY_DIR}/export_fixture.cpp")
file(WRITE "${FIXTURE_SOURCE}" "")
set(SYMBOL_INDEX 0)
foreach(SYMBOL IN LISTS ALL_FIXTURE_SYMBOLS)
  math(EXPR SYMBOL_INDEX "${SYMBOL_INDEX} + 1")
  file(APPEND "${FIXTURE_SOURCE}"
    "extern \"C\" void fixture_${SYMBOL_INDEX}() asm(\"${SYMBOL}\");\n"
    "extern \"C\" void fixture_${SYMBOL_INDEX}() {}\n")
endforeach()

function(verify_policy COMPILER POLICY SCRIPT EXPECT_CPP EXPECT_MMAP)
  get_filename_component(COMPILER_NAME "${COMPILER}" NAME)
  set(OUTPUT_DIR "${TEST_BINARY_DIR}/${COMPILER_NAME}-${POLICY}")
  file(MAKE_DIRECTORY "${OUTPUT_DIR}")
  set(LIBRARY "${OUTPUT_DIR}/libexport_fixture.so")

  execute_process(
    COMMAND "${COMPILER}" -shared -fPIC -nostdlib
            "-Wl,--version-script=${REPO_ROOT}/${SCRIPT}"
            "${FIXTURE_SOURCE}" -o "${LIBRARY}"
    RESULT_VARIABLE COMPILE_RESULT
    OUTPUT_VARIABLE COMPILE_STDOUT
    ERROR_VARIABLE COMPILE_STDERR)
  if(NOT COMPILE_RESULT EQUAL 0)
    message(FATAL_ERROR
      "${COMPILER_NAME}/${POLICY} fixture link failed:\n"
      "${COMPILE_STDOUT}${COMPILE_STDERR}")
  endif()

  execute_process(
    COMMAND "${NM_TOOL}" -D --defined-only --format=posix "${LIBRARY}"
    RESULT_VARIABLE NM_RESULT
    OUTPUT_VARIABLE NM_OUTPUT
    ERROR_VARIABLE NM_ERROR)
  if(NOT NM_RESULT EQUAL 0)
    message(FATAL_ERROR
      "${COMPILER_NAME}/${POLICY} symbol inspection failed: ${NM_ERROR}")
  endif()

  set(ACTUAL_EXPORTS)
  string(REPLACE "\n" ";" NM_LINES "${NM_OUTPUT}")
  foreach(LINE IN LISTS NM_LINES)
    if(LINE MATCHES "^([^ ]+) ")
      list(APPEND ACTUAL_EXPORTS "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  list(SORT ACTUAL_EXPORTS)

  set(EXPECTED_EXPORTS ${COMMON_EXPORTS})
  if(NOT EXPECT_MMAP)
    list(REMOVE_ITEM EXPECTED_EXPORTS mmap mmap64 mremap munmap)
  endif()
  if(EXPECT_CPP)
    list(APPEND EXPECTED_EXPORTS ${CPP_EXPORTS})
  endif()
  list(SORT EXPECTED_EXPORTS)

  if(NOT ACTUAL_EXPORTS STREQUAL EXPECTED_EXPORTS)
    message(FATAL_ERROR
      "${COMPILER_NAME}/${POLICY} export mismatch\n"
      "expected: ${EXPECTED_EXPORTS}\nactual: ${ACTUAL_EXPORTS}")
  endif()

  foreach(PRIVATE_SYMBOL IN ITEMS
          helper_private
          _ZN11unwindstack12HiddenSymbolEv
          _ZN7android4base12HiddenSymbolEv
          backtrace_private)
    if(PRIVATE_SYMBOL IN_LIST ACTUAL_EXPORTS)
      message(FATAL_ERROR
        "${COMPILER_NAME}/${POLICY} leaked ${PRIVATE_SYMBOL}")
    endif()
  endforeach()
endfunction()

foreach(COMPILER IN LISTS COMPILERS)
  verify_policy("${COMPILER}" android version_script.ld FALSE TRUE)
  verify_policy("${COMPILER}" linux version_script.ld FALSE TRUE)
  verify_policy("${COMPILER}" ohos version_script_ohos.ld TRUE FALSE)
  verify_policy("${COMPILER}" ohos-mmap version_script_ohos_mmap.ld TRUE TRUE)
endforeach()

message(STATUS "Verified platform export policies with: ${COMPILERS}")
