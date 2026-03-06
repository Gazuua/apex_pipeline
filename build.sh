#!/bin/bash
set -e

PRESET="${1:-debug}"

if [ -z "$VCPKG_ROOT" ]; then
    echo "Error: VCPKG_ROOT is not set"
    exit 1
fi

cd "$(dirname "$0")"

# Configure only if build directory doesn't exist yet
if [ ! -f "build/$PRESET/build.ninja" ]; then
    echo "[build.sh] Configuring preset: $PRESET"
    cmake --preset "$PRESET"
else
    echo "[build.sh] Build directory exists, skipping configure"
fi

cmake --build "build/$PRESET"
ctest --test-dir "build/$PRESET" --output-on-failure
