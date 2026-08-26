if(NOT DEFINED BUILD_DIR)
  message(FATAL_ERROR "BUILD_DIR is required")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target unwindstack
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
  message(FATAL_ERROR
    "Accurate unwindstack target failed to build:\n${stdout}\n${stderr}")
endif()
