# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

# ── Build stage ──────────────────────────────────────
ARG CI_IMAGE_TAG=latest
FROM ghcr.io/gazuua/apex-pipeline-ci:${CI_IMAGE_TAG} AS builder

ARG CMAKE_TARGET
ARG SERVICE_DIR
COPY . /src
WORKDIR /src
RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake --preset debug -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed \
          -DCMAKE_CXX_COMPILER_LAUNCHER=sccache \
          -DCMAKE_C_COMPILER_LAUNCHER=sccache \
    && cmake --build build/Linux/debug --target ${CMAKE_TARGET} \
    && mkdir -p /out \
    && cp build/Linux/debug/apex_services/${SERVICE_DIR}/${CMAKE_TARGET} /out/${CMAKE_TARGET}

# ── Runtime stage ────────────────────────────────────
FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libpq5 libsasl2-2 libzstd1 libcurl4 netcat-openbsd \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system apex && useradd --system --gid apex --no-create-home apex
ARG CMAKE_TARGET
ARG CONFIG_FILE
ENV APP_BINARY=/app/${CMAKE_TARGET}
COPY --from=builder /out/${CMAKE_TARGET} /app/${CMAKE_TARGET}
COPY apex_services/tests/e2e/${CONFIG_FILE} /app/config.toml
RUN chown -R apex:apex /app
WORKDIR /app
USER apex
ENTRYPOINT ["/bin/sh", "-c", "exec $APP_BINARY config.toml"]

# ── Valgrind stage (nightly) ─────────────────────────
FROM runtime AS valgrind
USER root
RUN apt-get update && apt-get install -y --no-install-recommends valgrind \
    && rm -rf /var/lib/apt/lists/*
USER apex
ENTRYPOINT ["/bin/sh", "-c", "exec valgrind --tool=memcheck --leak-check=full --error-exitcode=1 $APP_BINARY config.toml"]
