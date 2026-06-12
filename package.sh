#!/usr/bin/env bash

set -euo pipefail

if [[ -z ${ANDROID_NDK_HOME-} ]]; then
    echo "error: ANDROID_NDK_HOME variable is required to build this project"
    exit 1
fi

# change these to your Android NDK configuration
# ----------------------------------------------
API_LEVEL=21        # see https://apilevels.com/
COMPILER="clang"    # usually clang
COMPILER_VERSION=20 # not ndk version! (check by running the compiler on the NDK path)
# ----------------------------------------------

STRIP_BINARY="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

# based on https://developer.android.com/ndk/guides/abis
SUPPORTED_ARCH=("armv7" "armv8" "x86" "x86_64")
SUPPORTED_ABI=("armeabi-v7a" "arm64-v8a" "x86" "x86_64")

CONAN_PROFILE="${1-default}"

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
PACKAGE_DIR="build/package"

# Build server binaries in subshell
# ---------------------------------
build_server() (
    cd madbfs-server

    for i in $(seq ${#SUPPORTED_ARCH[@]}); do
        arch=${SUPPORTED_ARCH[$i - 1]}
        abi=${SUPPORTED_ABI[$i - 1]}

        echo "----------------------[ building server for ${arch} ]----------------------"

        conan install . \
            --build missing \
            -pr:b ${CONAN_PROFILE} \
            -s:b compiler.cppstd=23 \
            -c:h tools.android:ndk_path="${ANDROID_NDK_HOME/#\~/$HOME}" \
            -s:h arch=${arch} \
            -s:h build_type=Release \
            -s:h compiler=${COMPILER} \
            -s:h compiler.cppstd=23 \
            -s:h compiler.libcxx=c++_static \
            -s:h compiler.version=${COMPILER_VERSION} \
            -s:h os=Android \
            -s:h os.api_level=${API_LEVEL}

        cmake --preset conan-android-${arch}-release
        cmake --build --preset conan-android-${arch}-release

        "${STRIP_BINARY/#\~/$HOME}" "./build/android-${arch}-release/madbfs-server"
    done
)

# Build client
# ------------
build_madbfs() {
    echo "-------------------------[ begin build madbfs ]--------------------------"

    mkdir -p "${PACKAGE_DIR}/madbfs/"

    # copy madbfs servers to package dir
    for i in $(seq ${#SUPPORTED_ARCH[@]}); do
        arch=${SUPPORTED_ARCH[$i - 1]}
        abi=${SUPPORTED_ABI[$i - 1]}

        cp "./madbfs-server/build/android-${arch}-release/madbfs-server" "${PACKAGE_DIR}/madbfs-server-${abi}"
    done

    # build madbfs client and ipc client and run test
    conan install . --build=missing -pr:h "$CONAN_PROFILE" -pr:b "$CONAN_PROFILE"
    cmake --preset conan-release -D MADBFS_SERVER_BINARY_DIR="$(realpath "${PACKAGE_DIR}")"
    cmake --build --preset conan-release
    ctest --preset conan-release --verbose

    # strip and copy madbfs client to package dir
    strip "./build/Release/madbfs/madbfs"
    cp "./build/Release/madbfs/madbfs" "${PACKAGE_DIR}/madbfs/"

    strip "./build/Release/madbfs-msg/madbfs-msg"
    cp "./build/Release/madbfs-msg/madbfs-msg" "${PACKAGE_DIR}/madbfs/"
}

main() {
    cd ${SCRIPT_DIR}
    echo "building madbfs and madbfs-server using build profile: ${CONAN_PROFILE}"

    build_server
    build_madbfs

    # package
    cd ${PACKAGE_DIR}
    tar -czvf madbfs.tar.gz madbfs/

    echo "---------------------------[ build complete ]----------------------------"
    echo "package is built: ${PACKAGE_DIR}/madbfs.tar.gz"
}

main
