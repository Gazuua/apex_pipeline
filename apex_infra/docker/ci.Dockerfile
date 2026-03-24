# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
ARG TOOLS_TAG=latest
FROM ghcr.io/gazuua/apex-pipeline-tools:${TOOLS_TAG}

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

ENV VCPKG_INSTALLED_DIR=/opt/vcpkg_installed

WORKDIR /workspace
