#!/bin/bash
set -e

# ── Minimum required versions ──────────────────────
REQUIRED_CMAKE_MAJOR=3
REQUIRED_CMAKE_MINOR=25
REQUIRED_GCC_MAJOR=14
REQUIRED_NINJA_MAJOR=1
REQUIRED_NINJA_MINOR=11

# ── Helper functions ───────────────────────────────
die() { echo "Error: $*" >&2; exit 1; }

check_command() {
    command -v "$1" >/dev/null 2>&1 || die "$1 not found (required: $2)"
}

version_ge() {
    # Returns 0 if $1.$2 >= $3.$4
    if [ "$1" -gt "$3" ]; then return 0; fi
    if [ "$1" -eq "$3" ] && [ "$2" -ge "$4" ]; then return 0; fi
    return 1
}

# ── Pre-flight checks ─────────────────────────────
check_command cmake "cmake >= ${REQUIRED_CMAKE_MAJOR}.${REQUIRED_CMAKE_MINOR}"
check_command ninja "ninja >= ${REQUIRED_NINJA_MAJOR}.${REQUIRED_NINJA_MINOR}"
check_command g++-14 "g++ >= ${REQUIRED_GCC_MAJOR}"

# cmake version check
CMAKE_VERSION=$(cmake --version | head -1 | grep -oP '\d+\.\d+' | head -1)
CMAKE_MAJ=${CMAKE_VERSION%%.*}
CMAKE_MIN=${CMAKE_VERSION##*.}
version_ge "$CMAKE_MAJ" "$CMAKE_MIN" "$REQUIRED_CMAKE_MAJOR" "$REQUIRED_CMAKE_MINOR" \
    || die "cmake ${CMAKE_VERSION} found, but >= ${REQUIRED_CMAKE_MAJOR}.${REQUIRED_CMAKE_MINOR} required"

# ninja version check
NINJA_VERSION=$(ninja --version)
NINJA_MAJ=${NINJA_VERSION%%.*}
NINJA_REST=${NINJA_VERSION#*.}
NINJA_MIN=${NINJA_REST%%.*}
version_ge "$NINJA_MAJ" "$NINJA_MIN" "$REQUIRED_NINJA_MAJOR" "$REQUIRED_NINJA_MINOR" \
    || die "ninja ${NINJA_VERSION} found, but >= ${REQUIRED_NINJA_MAJOR}.${REQUIRED_NINJA_MINOR} required"

# gcc version check
GCC_VERSION=$(g++-14 -dumpversion)
GCC_MAJ=${GCC_VERSION%%.*}
[ "$GCC_MAJ" -ge "$REQUIRED_GCC_MAJOR" ] \
    || die "g++ ${GCC_VERSION} found, but >= ${REQUIRED_GCC_MAJOR} required"

# VCPKG_ROOT check
[ -n "$VCPKG_ROOT" ] || die "VCPKG_ROOT is not set"
[ -d "$VCPKG_ROOT" ] || die "VCPKG_ROOT path does not exist: $VCPKG_ROOT"

# ── vcpkg binary caching ──────────────────────────
export VCPKG_BINARY_SOURCES="clear;files,${HOME}/.cache/vcpkg/archives,readwrite"

# ── Build ──────────────────────────────────────────
PRESET="${1:-debug}"
BUILD_DIR="build/$(uname -s)/$PRESET"

cd "$(dirname "$0")"

# Ensure build dir and compile_commands.json exist for first configure (clangd)
mkdir -p "$BUILD_DIR"
[ -f "$BUILD_DIR/compile_commands.json" ] || touch "$BUILD_DIR/compile_commands.json"

# Configure
echo "[build.sh] Configuring preset: $PRESET"
cmake --preset "$PRESET"

# Copy compile_commands.json to project root for clangd
cp "$BUILD_DIR/compile_commands.json" compile_commands.json 2>/dev/null || true

# Build
cmake --build "$BUILD_DIR"

# Test
ctest --preset "$PRESET" --output-on-failure
