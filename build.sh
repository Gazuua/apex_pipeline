#!/bin/bash
set -e

PRESET="${1:-debug}"

if [ -z "$VCPKG_ROOT" ]; then
    echo "Error: VCPKG_ROOT is not set"
    exit 1
fi

cd "$(dirname "$0")"
cmake --preset "$PRESET"
cmake --build "build/$PRESET"
ctest --test-dir "build/$PRESET" --output-on-failure
