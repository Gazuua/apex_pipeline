# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Build tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-14 cmake ninja-build git curl zip unzip tar pkg-config ca-certificates python3 \
    make linux-libc-dev perl \
    autoconf automake libtool bison flex libreadline-dev \
    && rm -rf /var/lib/apt/lists/*

# Set GCC 14 as default
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100

# sccache (compile cache — GCC + MSVC cross-platform support)
ARG SCCACHE_VERSION=0.10.0
RUN curl -fsSL "https://github.com/mozilla/sccache/releases/download/v${SCCACHE_VERSION}/sccache-v${SCCACHE_VERSION}-x86_64-unknown-linux-musl.tar.gz" \
    | tar xz --strip-components=1 -C /usr/local/bin/ "sccache-v${SCCACHE_VERSION}-x86_64-unknown-linux-musl/sccache" \
    && chmod +x /usr/local/bin/sccache

# vcpkg (pinned to builtin-baseline)
ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_FORCE_SYSTEM_BINARIES=1
RUN git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT \
    && cd $VCPKG_ROOT \
    && git checkout b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01 \
    && ./bootstrap-vcpkg.sh -disableMetrics

# Fixed binary cache path — GitHub Actions overrides HOME to /github/home,
# so the default ~/.cache/vcpkg/archives becomes unreachable in CI.
# By pinning to /opt/vcpkg_cache, CI jobs can restore pre-built binaries
# instead of rebuilding packages from source.
ENV VCPKG_DEFAULT_BINARY_CACHE=/opt/vcpkg_cache
RUN mkdir -p $VCPKG_DEFAULT_BINARY_CACHE

ENV CC=gcc-14 CXX=g++-14

WORKDIR /workspace
