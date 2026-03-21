# ── Build stage (self-contained, 로컬/외부 빌드용) ──
ARG CI_IMAGE_TAG
FROM ghcr.io/gazuua/apex-pipeline-ci:${CI_IMAGE_TAG} AS builder
COPY . /src
WORKDIR /src
RUN cmake --preset debug -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed \
    && cmake --build build/Linux/debug --target chat_svc_main \
    && mkdir -p /out \
    && cp build/Linux/debug/apex_services/chat-svc/chat_svc_main /out/chat_svc_main

# ── Runtime stage ──
FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libpq5 libsasl2-2 libzstd1 libcurl4 netcat-openbsd \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system apex && useradd --system --gid apex --no-create-home apex
COPY --from=builder /out/chat_svc_main /app/chat_svc_main
COPY apex_services/tests/e2e/chat_svc_e2e.toml /app/config.toml
RUN chown -R apex:apex /app
WORKDIR /app
USER apex
ENTRYPOINT ["/app/chat_svc_main", "config.toml"]

# ── Valgrind stage (nightly용) ──
FROM runtime AS valgrind
USER root
RUN apt-get update && apt-get install -y --no-install-recommends valgrind \
    && rm -rf /var/lib/apt/lists/*
USER apex
ENTRYPOINT ["valgrind", "--tool=memcheck", "--leak-check=full", \
            "--error-exitcode=1", \
            "/app/chat_svc_main", "config.toml"]
