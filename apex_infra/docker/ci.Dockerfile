FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Build tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-14 cmake ninja-build git curl zip unzip tar pkg-config ca-certificates python3 \
    make linux-libc-dev perl \
    && rm -rf /var/lib/apt/lists/*

# Set GCC 14 as default
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100

# vcpkg (pinned to builtin-baseline)
ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_FORCE_SYSTEM_BINARIES=1
RUN git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT \
    && cd $VCPKG_ROOT \
    && git checkout b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01 \
    && ./bootstrap-vcpkg.sh -disableMetrics

# Pre-install vcpkg dependencies (warms binary cache for CI)
COPY vcpkg.json /tmp/vcpkg-manifest/vcpkg.json
COPY apex_core/vcpkg.json /tmp/vcpkg-core/vcpkg.json
COPY apex_shared/vcpkg.json /tmp/vcpkg-shared/vcpkg.json
RUN $VCPKG_ROOT/vcpkg install \
    --x-manifest-root=/tmp/vcpkg-manifest \
    --x-install-root=/opt/vcpkg_installed \
    && $VCPKG_ROOT/vcpkg install \
    --x-manifest-root=/tmp/vcpkg-core \
    --x-install-root=/opt/vcpkg_installed \
    && $VCPKG_ROOT/vcpkg install \
    --x-manifest-root=/tmp/vcpkg-shared \
    --x-install-root=/opt/vcpkg_installed \
    && rm -rf /tmp/vcpkg-manifest /tmp/vcpkg-core /tmp/vcpkg-shared

ENV CC=gcc-14 CXX=g++-14 \
    VCPKG_INSTALLED_DIR=/opt/vcpkg_installed

WORKDIR /workspace
