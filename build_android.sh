#!/usr/bin/env bash
set -ex

BUILD_ARCH="arm64-v8a"
if [ -n "$1" ]; then
    BUILD_ARCH=$1
fi
USER="alloc_test"

SRC_DIR=$(readlink -f "`dirname $0`")
cd ${SRC_DIR}

# check NDK_ROOT env variable
if [ -z "$NDK_ROOT" ]; then
    echo "Please set NDK_ROOT environment variable"
    exit 1
else
    echo "use $NDK_ROOT to build for android"
fi

echo "check NDK is invalid or not..."
if [ ! -f "$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang++" ]; then
    echo "NDK_ROOT is invalid"
    exit 1
fi

CMAKE_PARA_ANDROID="-DCMAKE_TOOLCHAIN_FILE=${NDK_ROOT}/build/cmake/android.toolchain.cmake \
	-DANDROID_NDK=${NDK_ROOT} \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_TOOLCHAIN=clang++ \
    -DANDROID_PLATFORM=android-21"

CMAKE_PARA_ARMV7="-DCMAKE_TOOLCHAIN_FILE=${NDK_ROOT}/build/cmake/android.toolchain.cmake \
    -DANDROID_NDK=${NDK_ROOT} \
    -DANDROID_ABI=armeabi-v7a \
    -DANDROID_TOOLCHAIN=clang++ \
    -DANDROID_PLATFORM=android-21"

CMAKE_PARA=" -DCMAKE_INSTALL_PREFIX=${SRC_DIR} \
    -DCMAKE_BUILD_TYPE=Release -G Ninja \
    -DMALLOC_HOOK_OPENCL_PROBE=${MALLOC_HOOK_OPENCL_PROBE:-OFF} \
    -DMALLOC_HOOK_BUILD_TESTS=${MALLOC_HOOK_BUILD_TESTS:-ON}"

function cmake_para_gen(){ 
    case ${BUILD_ARCH} in
        "arm64-v8a")
            CMAKE_PARA="${CMAKE_PARA} ${CMAKE_PARA_ANDROID}"
            ;;
        "armeabi-v7a")
            CMAKE_PARA="${CMAKE_PARA} ${CMAKE_PARA_ARMV7}"
            ;;
        *)
            echo "BUILD_ARCH must be either 'arm64-v8a' or 'armeabi-v7a'"
            exit 1
            ;;
    esac
}

function build_sdk(){ 
    rm -rf build
    rm -rf out
    mkdir build
    mkdir -p out/lib
    mkdir -p out/bin

    pushd .
    cd build
    cmake ${CMAKE_PARA} .. 
    ninja install -v
    popd
}

function test_func(){
    adb shell rm -rf /data/local/tmp/trace
    adb shell mkdir -p /data/local/tmp/trace
    adb shell rm -rf /data/local/tmp/${USER}
    adb shell mkdir -p /data/local/tmp/${USER}

    current_dir=$(pwd)
    adb push --sync ${current_dir}/out/lib/* /data/local/tmp/${USER}
    adb push --sync ${current_dir}/out/bin/* /data/local/tmp/${USER}

    adb exec-out "cd /data/local/tmp/${USER}; \
        unset LD_PRELOAD; \
        export LD_LIBRARY_PATH=.; \
        chmod 777 ./alloc_hook_test; \
        LD_PRELOAD=liballoc_hook.so ./alloc_hook_test"
}
cmake_para_gen
build_sdk
if [[ "${MALLOC_HOOK_RUN_DEVICE_TEST:-ON}" == "ON" ]]; then
    test_func
else
    echo "Skipping adb device test (MALLOC_HOOK_RUN_DEVICE_TEST=${MALLOC_HOOK_RUN_DEVICE_TEST})"
fi
