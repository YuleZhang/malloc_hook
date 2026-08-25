cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED REPO_ROOT OR NOT DEFINED BUILD_DIR OR NOT DEFINED TOOLCHAIN_FILE)
  message(FATAL_ERROR "REPO_ROOT, BUILD_DIR, and TOOLCHAIN_FILE are required")
endif()

if(NOT EXISTS "${TOOLCHAIN_FILE}")
  message(FATAL_ERROR "Configured toolchain file does not exist: ${TOOLCHAIN_FILE}")
endif()

file(MAKE_DIRECTORY "${BUILD_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${REPO_ROOT}" -B "${BUILD_DIR}"
          "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}"
          "-DANDROID_ABI=${ANDROID_ABI}"
          "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
          "-DOHOS_ARCH=${OHOS_ARCH}"
          "-DMALLOC_HOOK_BUILD_TESTS=OFF"
          "-DCMAKE_BUILD_TYPE=Debug"
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_stdout
  ERROR_VARIABLE configure_stderr)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "Toolchain configure failed:\n${configure_stdout}\n${configure_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target alloc_hook
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "Toolchain alloc_hook build failed:\n${build_stdout}\n${build_stderr}")
endif()

message(STATUS "Toolchain build gate passed: ${TOOLCHAIN_FILE}")
