#!/usr/bin/env bash

set -e

if [[ -z $ANDROID_NDK_HOME ]]; then
    echo "error: ANDROID_NDK_HOME variable is required to build this project"
    exit 1
fi

# change these to your Android NDK configuration
# ----------------------------------------------
# ANDROID_NDK_PATH="~/Android/Sdk/ndk/29.0.13113456"  # https://developer.android.com/studio/projects/install-ndk or https://developer.android.com/ndk/downloads
ANDROID_NDK_PATH=${ANDROID_NDK_HOME}
API_LEVEL=21                                        # see https://apilevels.com/
COMPILER="clang"                                    # usually clang
COMPILER_VERSION=20                                 # not ndk version! (check by running the compiler on the NDK path)
# ----------------------------------------------

STRIP_BINARY="${ANDROID_NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

# based on https://developer.android.com/ndk/guides/abis
SUPPORTED_ARCH=("armv7" "armv8" "x86" "x86_64")
SUPPORTED_ABI=("armeabi-v7a" "arm64-v8a" "x86" "x86_64")

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"

cd ${SCRIPT_DIR}

mkdir -p build/android-all-release

for i in $(seq ${#SUPPORTED_ARCH[@]}); do
    arch=${SUPPORTED_ARCH[$i-1]}
    abi=${SUPPORTED_ABI[$i-1]}

    echo "----------------------[ building for ${arch} ]----------------------"

    conan install .                                                 \
        -b missing                                                  \
        -pr:b default                                               \
        -c:h tools.android:ndk_path="${ANDROID_NDK_PATH/#\~/$HOME}" \
        -s:h arch=${arch}                                           \
        -s:h build_type=Release                                     \
        -s:h compiler=${COMPILER}                                   \
        -s:h compiler.cppstd=23                                     \
        -s:h compiler.libcxx=c++_static                             \
        -s:h compiler.version=${COMPILER_VERSION}                   \
        -s:h os=Android                                             \
        -s:h os.api_level=${API_LEVEL}

    cmake --preset conan-android-${arch}-release
    cmake --build --preset conan-android-${arch}-release

    cp build/android-${arch}-release/madbfs-server build/android-all-release/madbfs-server-${abi}
    "${STRIP_BINARY/#\~/$HOME}" build/android-all-release/madbfs-server-${abi}
done
