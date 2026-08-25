cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED REPO_ROOT OR NOT DEFINED BUILD_DIR OR NOT DEFINED CXX_COMPILER)
  message(FATAL_ERROR "REPO_ROOT, BUILD_DIR, and CXX_COMPILER are required")
endif()

file(MAKE_DIRECTORY "${BUILD_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${REPO_ROOT}" -B "${BUILD_DIR}"
          "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
          "-DMALLOC_HOOK_BUILD_TESTS=OFF"
          "-DMALLOC_HOOK_ENABLE_RESOURCE_TRACKING=OFF"
          "-DCMAKE_EXPORT_COMPILE_COMMANDS=OFF"
          "-DCMAKE_BUILD_TYPE=Debug"
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_stdout
  ERROR_VARIABLE configure_stderr)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "Cross-platform ${CXX_COMPILER} configure failed:\n"
    "${configure_stdout}\n${configure_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target alloc_hook
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
    "Cross-platform ${CXX_COMPILER} build failed:\n"
    "${build_stdout}\n${build_stderr}")
endif()

message(STATUS "Cross-platform compiler gate passed: ${CXX_COMPILER}")
