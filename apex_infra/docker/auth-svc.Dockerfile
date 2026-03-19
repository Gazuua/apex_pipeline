# ── Build stage (self-contained, 로컬/외부 빌드용) ──
FROM ghcr.io/gazuua/apex-pipeline-ci:latest AS builder
COPY . /src
WORKDIR /src
RUN cmake --preset debug -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed \
    && cmake --build build/Linux/debug --target auth_svc_main \
    && mkdir -p /out \
    && cp build/Linux/debug/apex_services/auth-svc/auth_svc_main /out/auth_svc_main

# ── Runtime stage ──
FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libpq5 libsasl2-2 libzstd1 libcurl4 netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /out/auth_svc_main /app/auth_svc_main
COPY apex_services/tests/e2e/auth_svc_e2e.toml /app/config.toml
COPY apex_services/tests/keys/ /app/keys/
WORKDIR /app
ENTRYPOINT ["/app/auth_svc_main", "config.toml"]

# ── Valgrind stage (nightly용) ──
FROM runtime AS valgrind
RUN apt-get update && apt-get install -y --no-install-recommends valgrind \
    && rm -rf /var/lib/apt/lists/*
ENTRYPOINT ["valgrind", "--tool=memcheck", "--leak-check=full", \
            "--error-exitcode=1", \
            "/app/auth_svc_main", "config.toml"]
