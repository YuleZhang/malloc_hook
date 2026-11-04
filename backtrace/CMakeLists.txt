aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src SRC_LIST)
add_library(helper STATIC ${SRC_LIST})
target_include_directories(helper PUBLIC 
                            ${CMAKE_CURRENT_SOURCE_DIR}
                            ${CMAKE_CURRENT_SOURCE_DIR}/driver
                            ${CMAKE_CURRENT_SOURCE_DIR}/include
                        )
target_link_libraries(helper PUBLIC malloc_hook_platform_contract)
if(MALLOC_HOOK_TARGET_OS STREQUAL "android")
    target_include_directories(helper PRIVATE
                               ${CMAKE_CURRENT_SOURCE_DIR}/../unwindstack
                               ${CMAKE_CURRENT_SOURCE_DIR}/../unwindstack/include)
    target_compile_definitions(helper PRIVATE
        MALLOC_HOOK_ENABLE_LEGACY_UNWINDSTACK_ADAPTER=1)
    target_link_libraries(helper PRIVATE unwindstack)
endif()
target_compile_definitions(helper PRIVATE
                            "MALLOC_HOOK_ENABLE_DMA_CAPTURE=$<BOOL:${MALLOC_HOOK_ENABLE_DMA_CAPTURE}>")
# Internal-only seam for unit tests: production builds with tests disabled do
# not compile it, and version scripts hide the symbol even in test builds.
if(MALLOC_HOOK_BUILD_TESTS AND MALLOC_HOOK_ENABLE_DMA_CAPTURE
   AND MALLOC_HOOK_TARGET_OS STREQUAL "linux")
    target_compile_definitions(helper PRIVATE MALLOC_HOOK_DMA_ACCOUNTING_TEST=1)
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL Clang)
    target_compile_options(helper PRIVATE -fno-c++-static-destructors)
endif ()
