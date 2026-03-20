# ── Build stage (self-contained, 로컬/외부 빌드용) ──
FROM ghcr.io/gazuua/apex-pipeline-ci:latest AS builder
COPY . /src
WORKDIR /src
RUN cmake --preset debug -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed \
    && cmake --build build/Linux/debug --target apex_gateway \
    && mkdir -p /out \
    && cp build/Linux/debug/apex_services/gateway/apex_gateway /out/apex_gateway

# ── Runtime stage ──
FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libpq5 libsasl2-2 libzstd1 libcurl4 netcat-openbsd \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system apex && useradd --system --gid apex --no-create-home apex
COPY --from=builder /out/apex_gateway /app/apex_gateway
COPY apex_services/tests/e2e/gateway_e2e.toml /app/config.toml
COPY apex_services/tests/keys/ /app/keys/
RUN chown -R apex:apex /app
WORKDIR /app
USER apex
ENTRYPOINT ["/app/apex_gateway", "config.toml"]

# ── Valgrind stage (nightly용) ──
FROM runtime AS valgrind
USER root
RUN apt-get update && apt-get install -y --no-install-recommends valgrind \
    && rm -rf /var/lib/apt/lists/*
USER apex
ENTRYPOINT ["valgrind", "--tool=memcheck", "--leak-check=full", \
            "--error-exitcode=1", \
            "/app/apex_gateway", "config.toml"]
