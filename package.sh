#!/usr/bin/env bash

set -e

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
PACKAGE_DIR="build/package"

cd ${SCRIPT_DIR}

mkdir -p ${PACKAGE_DIR}/madbfs/

# build client
# ------------
conan install . --build=missing -s build_type=Release
cmake --preset conan-release
cmake --build --preset conan-release

cp build/Release/madbfs/madbfs ${PACKAGE_DIR}/madbfs/
strip ${PACKAGE_DIR}/madbfs/madbfs
# ------------

# build server
# ------------
./madbfs-server/build_all.sh

cp madbfs-server/build/android-all-release/* ${PACKAGE_DIR}/madbfs/
# ------------

# package
# -------
cd ${PACKAGE_DIR}
tar -czvf madbfs.tar.gz madbfs/

echo "package is built: ${PACKAGE_DIR}/madbfs.tar.gz"
# -------
