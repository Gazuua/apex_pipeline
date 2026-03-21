#!/bin/bash
set -e

PRESET="${1:-debug}"

# Pre-flight checks (cmake, ninja, gcc, vcpkg)
source "$(dirname "$0")/apex_tools/build-preflight.sh"

# Map uname to CMake ${hostSystemName}
case "$(uname -s)" in
    Linux*)           HOST_SYSTEM=Linux ;;
    Darwin*)          HOST_SYSTEM=Darwin ;;
    MINGW*|MSYS*|CYGWIN*) HOST_SYSTEM=Windows ;;
    *)                HOST_SYSTEM="$(uname -s)" ;;
esac
BUILD_DIR="build/$HOST_SYSTEM/$PRESET"

cd "$(dirname "$0")"

# Ensure build dir and compile_commands.json exist for first configure (clangd symlink)
mkdir -p "$BUILD_DIR"
[ -f "$BUILD_DIR/compile_commands.json" ] || touch "$BUILD_DIR/compile_commands.json"

# Always configure (cached runs are fast, ensures compile_commands.json stays fresh)
echo "[build.sh] Configuring preset: $PRESET"
cmake --preset "$PRESET"

# Copy compile_commands.json to project root for clangd (after configure generates it)
cp "$BUILD_DIR/compile_commands.json" compile_commands.json 2>/dev/null || true

cmake --build "$BUILD_DIR"
ctest --preset "$PRESET" --output-on-failure -LE e2e
