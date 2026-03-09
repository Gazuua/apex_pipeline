#!/bin/bash
set -e

PRESET="${1:-debug}"

if [ -z "$VCPKG_ROOT" ]; then
    echo "Error: VCPKG_ROOT is not set"
    exit 1
fi

cd "$(dirname "$0")"

# Ensure build dir and compile_commands.json exist for first configure (clangd symlink)
mkdir -p "build/$(uname -s)/$PRESET"
[ -f "build/$(uname -s)/$PRESET/compile_commands.json" ] || touch "build/$(uname -s)/$PRESET/compile_commands.json"

# Always configure (cached runs are fast, ensures compile_commands.json stays fresh)
echo "[build.sh] Configuring preset: $PRESET"
cmake --preset "$PRESET"

# Copy compile_commands.json to project root for clangd (after configure generates it)
cp "build/$(uname -s)/$PRESET/compile_commands.json" compile_commands.json 2>/dev/null || true

cmake --build "build/$(uname -s)/$PRESET"
ctest --preset "$PRESET" --output-on-failure
