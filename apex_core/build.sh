#!/bin/bash
set -e

PRESET="${1:-debug}"

if [ -z "$VCPKG_ROOT" ]; then
    echo "Error: VCPKG_ROOT is not set"
    exit 1
fi

cd "$(dirname "$0")"

# Always configure (cached runs are fast, ensures compile_commands.json stays fresh)
echo "[build.sh] Configuring preset: $PRESET"
cmake --preset "$PRESET"

cmake --build "build/$PRESET"
ctest --test-dir "build/$PRESET" --output-on-failure
