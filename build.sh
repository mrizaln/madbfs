#!/usr/bin/env bash

set -euo pipefail

CONAN_PROFILE="${1-default}"

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"

echo "-------------------------[ begin build madbfs ]--------------------------"

conan install . --build=missing -pr:h "$CONAN_PROFILE" -pr:b "$CONAN_PROFILE"
cmake --preset conan-release
cmake --build --preset conan-release
ctest --preset conan-release --verbose
