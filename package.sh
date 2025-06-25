#!/usr/bin/env bash

set -e

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"

cd ${SCRIPT_DIR}

mkdir -p build/package/madbfs/

# build client
# ------------
conan install . --build=missing -s build_type=Release
cmake --preset conan-release
cmake --build --preset conan-release

cp build/Release/madbfs/madbfs build/package/madbfs/
strip build/package/madbfs/madbfs
# ------------

# build server
# ------------
./madbfs-server/build_all.sh

cp madbfs-server/build/android-all-release/* build/package/madbfs/
# ------------

# package
# -------
tar -czvf build/package/madbfs.tar.gz build/package/madbfs/

echo "package is built: build/package/madbfs/madbfs.tar.gz"
# -------
