cmake_minimum_required(VERSION 3.23)
if(NOT DEFINED REPO_ROOT)
  message(FATAL_ERROR "REPO_ROOT is required")
endif()
find_program(BASH_EXECUTABLE bash REQUIRED)
set(SCRIPTS
    build.sh
    scripts/build/android.sh
    scripts/build/linux.sh
    scripts/build/ohos.sh)
foreach(script IN LISTS SCRIPTS)
  execute_process(
    COMMAND "\${BASH_EXECUTABLE}" -n "\${REPO_ROOT}/\${script}"
    RESULT_VARIABLE syntax_result
    OUTPUT_VARIABLE syntax_out
    ERROR_VARIABLE syntax_err)
  if(NOT syntax_result EQUAL 0)
    message(FATAL_ERROR "bash -n failed for \${script}:\n\${syntax_out}\${syntax_err}")
  endif()
endforeach()

execute_process(
  COMMAND "\${BASH_EXECUTABLE}" "\${REPO_ROOT}/build.sh"
  RESULT_VARIABLE no_os_result
  OUTPUT_VARIABLE no_os_out
  ERROR_VARIABLE no_os_err)
if(NOT no_os_result EQUAL 2 OR
   NOT "\${no_os_out}\${no_os_err}" MATCHES "target OS is required")
  message(FATAL_ERROR "build.sh must reject a missing OS with status 2")
endif()

execute_process(
  COMMAND "\${BASH_EXECUTABLE}" "\${REPO_ROOT}/build.sh" unsupported
  RESULT_VARIABLE unknown_result
  OUTPUT_VARIABLE unknown_out
  ERROR_VARIABLE unknown_err)
if(NOT unknown_result EQUAL 2 OR
   NOT "\${unknown_out}\${unknown_err}" MATCHES "Unsupported OS")
  message(FATAL_ERROR "build.sh must reject an unknown OS with status 2")
endif()

execute_process(
  COMMAND "\${BASH_EXECUTABLE}" "\${REPO_ROOT}/build.sh" --help
  RESULT_VARIABLE help_result
  OUTPUT_VARIABLE help_out
  ERROR_VARIABLE help_err)
if(NOT help_result EQUAL 0 OR NOT help_out MATCHES "android" OR
   NOT help_out MATCHES "linux" OR NOT help_out MATCHES "ohos")
  message(FATAL_ERROR "build.sh --help must list all supported OS targets")
endif()

message(STATUS "Build dispatcher syntax and mandatory-OS contract passed")
