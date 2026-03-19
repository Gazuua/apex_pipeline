# ── Build stage (self-contained, 로컬/외부 빌드용) ──
FROM ghcr.io/gazuua/apex-pipeline-ci:latest AS builder
COPY . /src
WORKDIR /src
RUN cmake --preset debug -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed \
    && cmake --build build/Linux/debug --target apex_gateway

# ── Runtime stage ──
# ARG PREBUILT_BIN: CI에서 사전 빌드된 바이너리 경로. 설정 시 builder stage 결과물 대신 사용.
FROM ubuntu:24.04 AS runtime
ARG PREBUILT_BIN=""
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libpq5 libsasl2-2 libzstd1 libcurl4 netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*
COPY ${PREBUILT_BIN:-build/Linux/debug/bin/apex_gateway} /app/apex_gateway
COPY apex_services/tests/e2e/gateway_e2e.toml /app/config.toml
COPY apex_services/tests/keys/ /app/keys/
WORKDIR /app
ENTRYPOINT ["/app/apex_gateway", "config.toml"]

# ── Valgrind stage (nightly용) ──
FROM runtime AS valgrind
RUN apt-get update && apt-get install -y --no-install-recommends valgrind \
    && rm -rf /var/lib/apt/lists/*
ENTRYPOINT ["valgrind", "--tool=memcheck", "--leak-check=full", \
            "--error-exitcode=1", \
            "/app/apex_gateway", "config.toml"]
