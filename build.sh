#!/usr/bin/env bash

set -euo pipefail

if ! [[ -v ANDROID_NDK_HOME ]]; then
    echo "$0: ANDROID_NDK_HOME variable not set (required to build this project)"
    exit 1
fi

ANDROID_NDK_HOME="${ANDROID_NDK_HOME/#\~/$HOME}"
STRIP_BINARY="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

# based on https://developer.android.com/ndk/guides/abis
SUPPORTED_ARCH=("armv7" "armv8" "x86" "x86_64")
SUPPORTED_ABI=("armeabi-v7a" "arm64-v8a" "x86" "x86_64")

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
PACKAGE_DIR="${SCRIPT_DIR}/build/package"

DEFAULT_PROFILE_CLIENT="${SCRIPT_DIR}/.github/workflows/profile-client.ini"
DEFAULT_PROFILE_SERVER="${SCRIPT_DIR}/.github/workflows/profile-server.ini"

COLUMNS=$(tput cols 2>/dev/null || echo 80)

center_echo() {
    local text="$@"
    local width="$COLUMNS"
    local pad_len=$(((width - ${#text}) / 2 - 4))

    local pad=""
    for ((i = 0; i < pad_len; i++)); do pad+="="; done

    # Render final string
    printf "%s[ %s ]%s\n" "$pad" "$text" "$pad"
}

# build server binaries in subshell
# ---------------------------------
# build_server <build_type> <profile_client> <profile_server>
#   build_type: Debug, Release, RelWithDebInfo
build_server() (
    local build_type="$1"
    local profile_client="$2"
    local profile_server="$3"

    cd madbfs-server

    local servers_path="./build/android-all-${build_type,,}"
    mkdir -p "${servers_path}"

    for i in ${!SUPPORTED_ARCH[@]}; do
        local arch=${SUPPORTED_ARCH[$i]}
        local abi=${SUPPORTED_ABI[$i]}

        center_echo "building server for ${arch}"

        # build: madbfs-server
        MADBFS_SERVER_ARCH="${arch}" \
            conan install . --build missing -pr:b "${profile_client}" -pr:h "${profile_server}" -s "&:build_type=${build_type}" -s build_type=Release
        cmake --preset conan-android-${arch}-${build_type,,}
        cmake --build --preset conan-android-${arch}-${build_type,,}

        if [[ "${build_type}" == "Release" ]]; then
            "${STRIP_BINARY}" "./build/android-${arch}-release/madbfs-server"
        fi

        # copy to centralized dir for preparation of building madbfs client
        cp "./build/android-${arch}-${build_type,,}/madbfs-server" "${servers_path}/madbfs-server-${abi}"
    done
)

# build client
# ------------
# build_server <build_type> <profile_client> <servers_path>
#   build_type: Debug, Release, RelWithDebInfo
build_client() {
    local build_type="$1"
    local profile_client="$2"
    local servers_path="$(realpath "$3")" # must be absolute path for #embed

    center_echo "build madbfs"

    # build: madbfs, madbfs-msg, test
    conan install . --build missing -pr:b "${profile_client}" -pr:h "${profile_client}" -s "&:build_type=${build_type}" -s build_type=Release
    cmake --preset conan-${build_type,,} -D MADBFS_SERVER_BINARY_DIR="${servers_path}"
    cmake --build --preset conan-${build_type,,}

    if [[ "${build_type}" == "Release" ]]; then
        strip "./build/Release/madbfs/madbfs"
        strip "./build/Release/madbfs-msg/madbfs-msg"
    fi

    # run test
    ctest --preset conan-${build_type,,} --verbose
}

# package as tar.gz
# -----------------
# package <build_type_client> <build_type_server>
package() (
    local target="${PACKAGE_DIR}-${1}-${2}"
    mkdir -p "${target}/madbfs"

    center_echo "package start"

    cp "./build/${1}/madbfs/madbfs" "${target}/madbfs"
    cp "./build/${1}/madbfs-msg/madbfs-msg" "${target}/madbfs"

    cd "${target}"

    tar -czvf madbfs.tar.gz madbfs/
    sha256sum madbfs.tar.gz >madbfs.tar.gz.sha256

    echo "package is built: ${target}/madbfs.tar.gz"
)

capitalize_build_type() {
    case "$1" in
    "debug" | "release") echo "${1^}" ;;
    "relwithdebinfo") echo "RelWithDebInfo" ;;
    *) echo "$1" ;;
    esac
}

main() {
    TEMP=$(getopt -o "h,t:,b:,p:" -l "help,target:,build-type:,profile:,package" -n "$0" -- "$@")

    eval set -- "$TEMP"
    unset TEMP

    local target="all"
    local build_type_client="Release"
    local build_type_server="Release"
    local profile_client="${DEFAULT_PROFILE_CLIENT}"
    local profile_server="${DEFAULT_PROFILE_SERVER}"
    local package=false

    while true; do
        case "$1" in
        "-h" | "--help")
            echo -e "usage: $0 [-t TARGET] [-b [TYPE][:TYPE]] [-p [PROFILE][:PROFILE]]\n"
            echo -e \
                "options:\n" \
                "    -h, --help            show this help\n" \
                "    -t, --target          target to build\n" \
                "                            (TARGET: 'all', 'client', 'server')\n" \
                "                            (default: 'all')\n" \
                "    -b, --build-type      build type of madbfs client and server (in order)\n" \
                "                            (useful when mixing build type of embedded server in client)\n" \
                "                            (TYPE   : 'debug', 'release', 'relwithdebinfo')\n" \
                "                            (default: 'release:release')\n" \
                "    -p, --profile         conan profile for building madbfs client and server (in order)\n" \
                "                            (PROFILE: path/to/conan/profile)\n" \
                "                            (default: $(realpath --relative-to . ${DEFAULT_PROFILE_CLIENT}):$(realpath --relative-to . ${DEFAULT_PROFILE_SERVER}))\n" \
                "    --package             create a tar archive of the resulting binaries\n"
            shift
            exit 0
            ;;
        "-t" | "--target")
            case "$2" in
            "all" | "client" | "server")
                target="$2"
                ;;
            *)
                echo "Unknown target"
                exit 1
                ;;
            esac

            shift 2
            continue
            ;;
        "-b" | "--build-type")
            IFS=":" read -r build_type_client build_type_server <<<"$2"

            build_type_client="$(capitalize_build_type ${build_type_client:-"release"})"
            build_type_server="$(capitalize_build_type ${build_type_server:-"release"})"

            shift 2
            continue
            ;;
        "-p" | "--profile")
            IFS=":" read -r profile_client profile_server <<<"$2"

            profile_client="${profile_client:-${DEFAULT_PROFILE_CLIENT}}"
            profile_server="${profile_server:-${DEFAULT_PROFILE_SERVER}}"

            shift 2
            continue
            ;;
        "--package")
            package=true
            shift 1
            continue
            ;;
        "--")
            shift
            break
            ;;
        *)
            echo "Unknown argument $1"
            exit 1
            ;;
        esac
    done

    if [[ $# -ge 1 ]]; then
        echo "$0: unknown arguments: $@"
        exit 1
    fi

    center_echo "build start"

    cd ${SCRIPT_DIR}

    local servers_path="./madbfs-server/build/android-all-${build_type_server,,}"

    case "${target}" in
    "all")
        echo -e "\tmadbfs        (build_type: ${build_type_client} | profile: ${profile_client})"
        echo -e "\tmadbfs-server (build_type: ${build_type_server} | profile: ${profile_server})"

        build_server "${build_type_server}" "${profile_client}" "${profile_server}"
        build_client "${build_type_client}" "${profile_client}" "${servers_path}"
        ;;
    "client")
        echo -e "\tmadbfs        (build_type: ${build_type_client} | profile: ${profile_client})"

        if [[ ! -d "${servers_path}" || -z "$(ls -A "${servers_path}")" ]]; then
            echo -e "error: madbfs-server with build type ${build_type_server} are not built yet"
            echo -e "error: build madbfs-server first by setting '--target' to 'all' or 'server'"
            exit 1
        fi

        build_client "${build_type_client}" "${profile_client}" "${servers_path}"
        ;;
    "server")
        echo -e "\tmadbfs-server (build_type: ${build_type_server} | profile: ${profile_server})"

        build_server "${build_type_server}" "${profile_client}" "${profile_server}"
        ;;
    esac

    if [[ ${package} == true ]]; then
        package "${build_type_client}" "${build_type_server}"
    fi

    center_echo "build complete"
}

main "$@"
