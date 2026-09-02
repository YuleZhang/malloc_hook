find_package(Threads REQUIRED)

# Several gates express their expectations with <cassert>. A build that defines
# NDEBUG (CMAKE_BUILD_TYPE=Release does) would compile those checks away and the
# gates would pass without verifying anything. Keep assertions live for every
# test target in this directory, independently of the selected build type.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-UNDEBUG)
endif()

if(MALLOC_HOOK_BUILD_GL_TESTS AND ANDROID)
    add_subdirectory(gl_test)
endif()

add_executable(hook_source_policy_test hook_source_policy_test.cpp)
target_compile_features(hook_source_policy_test PRIVATE cxx_std_17)
target_include_directories(hook_source_policy_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)

# Config.cpp reaches for <bionic/reserved_signals.h> to pick the default dump
# signal. That header is AOSP-internal and is not in the NDK sysroot; the
# library target resolves it through the vendored copy under unwindstack/. Any
# target that compiles Config.cpp for Bionic needs the same path, otherwise a
# cross build fails on a header rather than on anything under test.
set(MALLOC_HOOK_VENDORED_BIONIC_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/../unwindstack)

add_executable(config_capture_mode_test
    config_capture_mode_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/Config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/ObservedMemory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/debug_disable.cpp)
target_compile_features(config_capture_mode_test PRIVATE cxx_std_17)
target_include_directories(config_capture_mode_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
if(ANDROID)
    target_include_directories(config_capture_mode_test PRIVATE
        ${MALLOC_HOOK_VENDORED_BIONIC_INCLUDE})
endif()
target_link_libraries(config_capture_mode_test PRIVATE
    ${CMAKE_DL_LIBS} Threads::Threads)

add_test(NAME hook_source_policy COMMAND hook_source_policy_test)
add_test(NAME config_capture_mode COMMAND config_capture_mode_test)

add_executable(observed_memory_test
    observed_memory_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/Config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/ObservedMemory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/debug_disable.cpp)
target_compile_features(observed_memory_test PRIVATE cxx_std_17)
target_include_directories(observed_memory_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
if(ANDROID)
    target_include_directories(observed_memory_test PRIVATE
        ${MALLOC_HOOK_VENDORED_BIONIC_INCLUDE})
endif()
target_link_libraries(observed_memory_test PRIVATE
    ${CMAKE_DL_LIBS} Threads::Threads)
add_test(NAME observed_memory COMMAND observed_memory_test)

add_executable(sampling_test sampling_test.cpp)
target_compile_features(sampling_test PRIVATE cxx_std_17)
target_include_directories(sampling_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
add_test(NAME sampling COMMAND sampling_test)

add_executable(async_report_contract_test
    async_report_contract_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/AsyncStackPipeline.cpp)
target_compile_features(async_report_contract_test PRIVATE cxx_std_17)
target_include_directories(async_report_contract_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
target_link_libraries(async_report_contract_test PRIVATE
    ${CMAKE_DL_LIBS} Threads::Threads)
add_test(NAME async_report_contract COMMAND async_report_contract_test)

add_executable(pointer_stack_contract_test pointer_stack_contract_test.cpp)
target_compile_features(pointer_stack_contract_test PRIVATE cxx_std_17)
target_include_directories(pointer_stack_contract_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
target_link_libraries(pointer_stack_contract_test PRIVATE Threads::Threads)
add_test(NAME pointer_stack_contract COMMAND pointer_stack_contract_test)

add_executable(async_lifecycle_identity_test
    async_lifecycle_identity_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/AsyncStackPipeline.cpp)
target_compile_features(async_lifecycle_identity_test PRIVATE cxx_std_17)
target_include_directories(async_lifecycle_identity_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
target_link_libraries(async_lifecycle_identity_test PRIVATE
    ${CMAKE_DL_LIBS} Threads::Threads)
add_test(NAME async_lifecycle_identity COMMAND async_lifecycle_identity_test)
set_tests_properties(async_lifecycle_identity PROPERTIES TIMEOUT 10)

# Registered for every target, not just the host: the sharded tracker's whole
# point is contention between many allocating threads, and the platform that
# actually runs many of them is the device.
add_executable(pointer_shard_concurrency_test
    pointer_shard_concurrency_test.cpp)
target_compile_features(pointer_shard_concurrency_test PRIVATE cxx_std_17)
target_include_directories(pointer_shard_concurrency_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
target_link_libraries(pointer_shard_concurrency_test PRIVATE
    helper ${CMAKE_DL_LIBS} Threads::Threads)
add_test(NAME pointer_shard_concurrency COMMAND pointer_shard_concurrency_test)
# Well above the observed runtime; a wedged shard lock should surface as a
# timeout rather than hanging the suite.
set_tests_properties(pointer_shard_concurrency PROPERTIES TIMEOUT 60)

add_test(
    NAME pointer_stack_boundary
    COMMAND ${CMAKE_COMMAND}
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DREPO_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/.."
        "-DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/pointer_stack_boundary"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/pointer_stack_boundary_test.cmake)

if(NOT ANDROID AND NOT OHOS)
    add_executable(malloc_debug_semantics_test
        malloc_debug_semantics_test.cpp)
    target_compile_features(malloc_debug_semantics_test PRIVATE cxx_std_17)
    target_include_directories(malloc_debug_semantics_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
    target_link_libraries(malloc_debug_semantics_test PRIVATE
        helper Threads::Threads)
    add_test(NAME malloc_debug_semantics COMMAND malloc_debug_semantics_test)
    set_tests_properties(malloc_debug_semantics PROPERTIES TIMEOUT 10)

    add_executable(runtime_smoke_gate_test
        runtime_smoke_gate_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/AsyncStackPipeline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/UnwindBacktrace.cpp)
    target_compile_features(runtime_smoke_gate_test PRIVATE cxx_std_17)
    target_include_directories(runtime_smoke_gate_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
    target_compile_definitions(runtime_smoke_gate_test PRIVATE
        MALLOC_HOOK_ASYNC_STACK_TESTING=1
        MALLOC_HOOK_STACK_CAPTURE_TESTING=1)
    target_link_libraries(runtime_smoke_gate_test PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads
        malloc_hook_platform_contract)
    add_test(NAME runtime_smoke_gate COMMAND runtime_smoke_gate_test)
    set_tests_properties(runtime_smoke_gate PROPERTIES TIMEOUT 10)

    add_executable(task10_hook_boundary_test
        task10_hook_boundary_test.cpp)
    target_compile_features(task10_hook_boundary_test PRIVATE cxx_std_17)
    target_include_directories(task10_hook_boundary_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
    target_link_libraries(task10_hook_boundary_test PRIVATE
        helper Threads::Threads)
    add_test(NAME task10_hook_boundary COMMAND task10_hook_boundary_test)
    set_tests_properties(task10_hook_boundary PROPERTIES TIMEOUT 5)

    if(MALLOC_HOOK_TARGET_OS STREQUAL "linux")
        add_executable(linux_mmap_ioctl_regression_test
            linux_mmap_ioctl_regression_test.cpp)
        target_compile_features(linux_mmap_ioctl_regression_test PRIVATE cxx_std_17)
        target_include_directories(linux_mmap_ioctl_regression_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
        target_link_libraries(linux_mmap_ioctl_regression_test PRIVATE
            helper Threads::Threads)
        add_test(NAME linux_mmap_ioctl_regression
            COMMAND linux_mmap_ioctl_regression_test)
        set_tests_properties(linux_mmap_ioctl_regression PROPERTIES TIMEOUT 10)

        add_executable(mremap_lifecycle_test
            mremap_lifecycle_test.cpp)
        target_compile_features(mremap_lifecycle_test PRIVATE cxx_std_17)
        target_include_directories(mremap_lifecycle_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
        target_link_libraries(mremap_lifecycle_test PRIVATE
            helper Threads::Threads)
        add_test(NAME mremap_lifecycle COMMAND mremap_lifecycle_test)
        set_tests_properties(mremap_lifecycle PROPERTIES TIMEOUT 10)

        add_executable(fork_lock_safety_test
            fork_lock_safety_test.cpp)
        target_compile_features(fork_lock_safety_test PRIVATE cxx_std_17)
        target_include_directories(fork_lock_safety_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
        target_link_libraries(fork_lock_safety_test PRIVATE
            helper Threads::Threads)
        add_test(NAME fork_lock_safety COMMAND fork_lock_safety_test)
        # Above the in-test per-child deadline (which the test stops at on the
        # first hang) so a deadlock surfaces as the test's own assertion rather
        # than as an opaque ctest timeout.
        set_tests_properties(fork_lock_safety PROPERTIES TIMEOUT 60)
    endif()
endif()

if(MALLOC_HOOK_HAVE_GTEST)
    add_executable(stack_contract_test stack_contract_test.cpp)
    target_compile_features(stack_contract_test PRIVATE cxx_std_17)
    target_include_directories(stack_contract_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
    target_link_libraries(stack_contract_test PRIVATE gtest_main)
    add_test(NAME stack_contract COMMAND stack_contract_test)

    add_executable(linux_unwind_adapter_test
        linux_unwind_adapter_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/UnwindBacktrace.cpp)
    target_compile_features(linux_unwind_adapter_test PRIVATE cxx_std_17)
    target_include_directories(linux_unwind_adapter_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../unwindstack/include)
    target_link_libraries(linux_unwind_adapter_test PRIVATE
        gtest_main ${CMAKE_DL_LIBS} Threads::Threads
        malloc_hook_platform_contract)
    add_test(NAME linux_unwind_adapter COMMAND linux_unwind_adapter_test)

    # The legacy integration binary requires Android EGL/GLES and the local
    # OpenCL stub. Keep it out of portable Linux/OHOS test builds.
    if(ANDROID AND TARGET opencl-stub AND TARGET gles3jni)
        add_executable(alloc_hook_test
            alloc_hook_test.cpp
            util/utils.cpp)
        target_link_libraries(alloc_hook_test PRIVATE
            gtest opencl-stub gles3jni log)
        install(TARGETS alloc_hook_test DESTINATION ${CMAKE_INSTALL_PREFIX}/out/bin)
    endif()

    add_executable(async_stack_pipeline_test
        async_stack_pipeline_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/AsyncStackPipeline.cpp)
    target_compile_features(async_stack_pipeline_test PRIVATE cxx_std_17)
    target_include_directories(async_stack_pipeline_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include)
    target_link_libraries(async_stack_pipeline_test PRIVATE
        gtest_main ${CMAKE_DL_LIBS} Threads::Threads)

    add_executable(capture_routing_test
        capture_routing_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/UnwindBacktrace.cpp)
    target_compile_features(capture_routing_test PRIVATE cxx_std_17)
    target_include_directories(capture_routing_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../unwindstack/include)
    target_compile_definitions(capture_routing_test PRIVATE
        MALLOC_HOOK_STACK_CAPTURE_TESTING=1)
    if(ANDROID)
        target_compile_definitions(capture_routing_test PRIVATE
            MALLOC_HOOK_ENABLE_LEGACY_UNWINDSTACK_ADAPTER=1)
    endif()
    target_link_libraries(capture_routing_test PRIVATE
        gtest_main ${CMAKE_DL_LIBS} Threads::Threads
        malloc_hook_platform_contract)
    if(ANDROID)
        target_link_libraries(capture_routing_test PRIVATE unwindstack)
    endif()

    # Compiles the Linux capture path on purpose (MALLOC_HOOK_TARGET_OS_LINUX=1)
    # to exercise the no-compiler-unwind fallback. That path calls backtrace()
    # from <execinfo.h>, which neither Bionic nor musl provides, so this gate is
    # host-only by construction: cross-building it for a device target fails to
    # compile without saying anything about the code under test.
    if(NOT ANDROID AND NOT OHOS)
        add_executable(capture_fallback_test
            capture_routing_test.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/src/UnwindBacktrace.cpp)
        target_compile_features(capture_fallback_test PRIVATE cxx_std_17)
        target_include_directories(capture_fallback_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../backtrace/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../unwindstack/include)
        target_compile_definitions(capture_fallback_test PRIVATE
            MALLOC_HOOK_HAVE_COMPILER_UNWIND=0
            MALLOC_HOOK_TARGET_OS_LINUX=1
            MALLOC_HOOK_STACK_CAPTURE_TESTING=1)
        target_link_libraries(capture_fallback_test PRIVATE
            gtest_main ${CMAKE_DL_LIBS} Threads::Threads)
        add_test(NAME capture_fallback COMMAND capture_fallback_test)
    endif()

    add_test(NAME async_stack_pipeline COMMAND async_stack_pipeline_test)
    add_test(NAME capture_routing COMMAND capture_routing_test)
endif()
