# v0.6.2 Docker 멀티스테이지 빌드 고도화 — 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** CI 이미지 레이어 분리, Docker Bake 통합, sccache 도입, 보안 강화, MSVC CI 빌드 추가로 Docker 빌드 파이프라인을 고도화한다.

**Architecture:** 2단계 CI 이미지(tools-base→ci) + 단일 service.Dockerfile + docker-bake.hcl로 서비스 빌드 통합. sccache로 컴파일 캐시, BuildKit cache mount로 vcpkg/CMake 캐시. CI에 MSVC 빌드 잡 + Trivy 보안 스캔 추가.

**Tech Stack:** Docker BuildKit, Docker Bake (buildx), sccache, Trivy, GitHub Actions, CMake Presets

**Spec:** `docs/apex_infra/plans/20260325_094102_docker_multistage_build_design.md`

**스펙 대비 변경사항:**
- 스펙 3.3절의 `debug-sccache` 프리셋 외에 `release-sccache`도 추가 (MSVC CI 빌드에 필요)
- 스펙 4.4절의 `msvc-release-sccache` 프리셋명은 기존 `msvc-release` 프리셋이 없으므로 `release-sccache` (inherits `release`)로 대체
- 스펙 3.4절의 `.dockerignore` 블랙리스트 패턴은 기존 화이트리스트 패턴이 더 안전하므로 화이트리스트 유지
- `nightly.yml`은 호스트에서 직접 Valgrind를 실행하므로 Dockerfile 변경의 영향 없음 (수정 불필요)

---

## 파일 구조

### 신규 파일
| 파일 | 역할 |
|------|------|
| `apex_infra/docker/tools-base.Dockerfile` | 도구 베이스 이미지 (gcc, cmake, ninja, vcpkg CLI, sccache) |
| `apex_infra/docker/service.Dockerfile` | 통합 서비스 Dockerfile (ARG 파라미터화) |
| `apex_infra/docker/docker-bake.hcl` | Bake 타겟 매트릭스 정의 |
| `.github/workflows/docker-images.yml` | tools-base + ci 이미지 빌드 워크플로우 |
| `.github/tools-image-tag` | tools-base 이미지 태그 추적 파일 |

### 수정 파일
| 파일 | 변경 내용 |
|------|-----------|
| `apex_infra/docker/ci.Dockerfile` | tools-base 기반으로 개편 + vcpkg cache mount |
| `.dockerignore` | 화이트리스트 패턴 유지하며 sccache/bake 관련 조정 |
| `CMakePresets.json` | `debug-sccache`, `release-sccache` 프리셋 추가 |
| `.github/workflows/ci.yml` | build-msvc 잡 + bake-services 잡 + scan 잡 추가 |
| `apex_infra/docker/docker-compose.e2e.yml` | service.Dockerfile 참조 + RSA 키 volume mount |
| `apex_infra/docker/docker-compose.valgrind.yml` | service.Dockerfile 호환 args 추가 |

### 삭제 파일
| 파일 | 사유 |
|------|------|
| `apex_infra/docker/gateway.Dockerfile` | service.Dockerfile로 통합 |
| `apex_infra/docker/auth-svc.Dockerfile` | service.Dockerfile로 통합 |
| `apex_infra/docker/chat-svc.Dockerfile` | service.Dockerfile로 통합 |
| `.github/workflows/docker-build.yml` | docker-images.yml로 대체 |

---

## Task 1: .dockerignore 조정 + CMakePresets.json sccache 프리셋

**Files:**
- Modify: `.dockerignore`
- Modify: `CMakePresets.json`

- [ ] **Step 1: .dockerignore 업데이트**

기존 화이트리스트 패턴(`*` + `!` 예외) 유지. `build.bat`/`build.sh` 예외를 화이트리스트에서 제외 (Docker 빌드에 불필요):

```dockerignore
*
!vcpkg.json
!apex_core/
!apex_shared/
!apex_services/
!CMakeLists.txt
!CMakePresets.json
!cmake/
```

- [ ] **Step 2: CMakePresets.json에 sccache 프리셋 추가**

기존 프리셋을 불변으로 유지하고, inherits로 확장:

`configurePresets`에 추가:
```json
{
  "name": "debug-sccache",
  "displayName": "Debug + sccache",
  "inherits": "debug",
  "cacheVariables": {
    "CMAKE_C_COMPILER_LAUNCHER": "sccache",
    "CMAKE_CXX_COMPILER_LAUNCHER": "sccache"
  }
},
{
  "name": "release-sccache",
  "displayName": "Release + sccache",
  "inherits": "release",
  "cacheVariables": {
    "CMAKE_C_COMPILER_LAUNCHER": "sccache",
    "CMAKE_CXX_COMPILER_LAUNCHER": "sccache"
  }
}
```

`buildPresets`에 추가:
```json
{ "name": "debug-sccache", "configurePreset": "debug-sccache" },
{ "name": "release-sccache", "configurePreset": "release-sccache" }
```

`testPresets`에 추가:
```json
{
  "name": "debug-sccache",
  "configurePreset": "debug-sccache",
  "output": { "outputOnFailure": true }
}
```

- [ ] **Step 3: 커밋**

```bash
git add .dockerignore CMakePresets.json
git commit -m "chore(infra): .dockerignore 정리 + sccache CMake 프리셋 추가 (BACKLOG-176)"
git push
```

---

## Task 2: tools-base.Dockerfile 생성

**Files:**
- Create: `apex_infra/docker/tools-base.Dockerfile`
- Create: `.github/tools-image-tag`

- [ ] **Step 1: tools-base.Dockerfile 작성**

현재 `ci.Dockerfile`에서 도구 설치 부분만 추출. sccache 바이너리 추가:

```dockerfile
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

# sccache (compile cache — GCC + MSVC 지원)
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

# Fixed binary cache path
ENV VCPKG_DEFAULT_BINARY_CACHE=/opt/vcpkg_cache
RUN mkdir -p $VCPKG_DEFAULT_BINARY_CACHE

ENV CC=gcc-14 CXX=g++-14

WORKDIR /workspace
```

- [ ] **Step 2: .github/tools-image-tag 초기 파일 생성**

```
latest
```

> 첫 빌드 전까지 `latest`로 지정. docker-images.yml 워크플로우가 빌드 후 SHA로 갱신.

- [ ] **Step 3: 커밋**

```bash
git add apex_infra/docker/tools-base.Dockerfile .github/tools-image-tag
git commit -m "feat(infra): tools-base Docker 이미지 추가 — gcc14+cmake+ninja+sccache+vcpkg (BACKLOG-176)"
git push
```

---

## Task 3: ci.Dockerfile 개편 (tools-base 기반)

**Files:**
- Modify: `apex_infra/docker/ci.Dockerfile`

- [ ] **Step 1: ci.Dockerfile을 tools-base 기반으로 재작성**

```dockerfile
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
```

> **주의**: `TOOLS_TAG` ARG는 docker-images.yml에서 `.github/tools-image-tag` 값으로 주입. 기존 `ENV` 변수들(VCPKG_ROOT, CC, CXX 등)은 tools-base에서 상속.

- [ ] **Step 2: 커밋**

```bash
git add apex_infra/docker/ci.Dockerfile
git commit -m "refactor(infra): ci.Dockerfile → tools-base 기반으로 개편 (BACKLOG-176)"
git push
```

---

## Task 4: service.Dockerfile 생성 (통합)

**Files:**
- Create: `apex_infra/docker/service.Dockerfile`

- [ ] **Step 1: 통합 service.Dockerfile 작성**

3개 개별 Dockerfile 대체. 서비스별 차이는 ARG로 파라미터화:

```dockerfile
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

# ── Build stage ──
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

# ── Runtime stage ──
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

# ── Valgrind stage (nightly용) ──
FROM runtime AS valgrind
USER root
RUN apt-get update && apt-get install -y --no-install-recommends valgrind \
    && rm -rf /var/lib/apt/lists/*
USER apex
ENTRYPOINT ["/bin/sh", "-c", "exec valgrind --tool=memcheck --leak-check=full --error-exitcode=1 $APP_BINARY config.toml"]
```

> **파라미터 매핑 (docker-bake.hcl에서 주입)**:
> | 서비스 | CMAKE_TARGET | SERVICE_DIR | CONFIG_FILE |
> |--------|-------------|-------------|-------------|
> | gateway | `apex_gateway` | `gateway` | `gateway_e2e.toml` |
> | auth-svc | `auth_svc_main` | `auth-svc` | `auth_svc_e2e.toml` |
> | chat-svc | `chat_svc_main` | `chat-svc` | `chat_svc_e2e.toml` |

- [ ] **Step 2: 커밋**

```bash
git add apex_infra/docker/service.Dockerfile
git commit -m "feat(infra): 통합 service.Dockerfile — ARG 파라미터화 (BACKLOG-176)"
git push
```

---

## Task 5: docker-bake.hcl 생성

**Files:**
- Create: `apex_infra/docker/docker-bake.hcl`

- [ ] **Step 1: docker-bake.hcl 작성**

```hcl
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

variable "CI_IMAGE_TAG" {
  default = "latest"
}

variable "REGISTRY" {
  default = "ghcr.io/gazuua"
}

variable "SHA_TAG" {
  default = "latest"
}

// ── Service group ─────────────────────────────────
group "services" {
  targets = ["gateway", "auth-svc", "chat-svc"]
}

// ── Base target (공통 설정) ───────────────────────
target "service-base" {
  dockerfile = "docker/service.Dockerfile"
  context    = ".."
  cache-from = ["type=registry,ref=${REGISTRY}/apex-pipeline-cache:buildcache"]
  cache-to   = ["type=registry,ref=${REGISTRY}/apex-pipeline-cache:buildcache,mode=max"]
}

// ── Gateway ───────────────────────────────────────
target "gateway" {
  inherits = ["service-base"]
  args = {
    CMAKE_TARGET = "apex_gateway"
    SERVICE_DIR  = "gateway"
    CONFIG_FILE  = "gateway_e2e.toml"
    CI_IMAGE_TAG = CI_IMAGE_TAG
  }
  tags = [
    "${REGISTRY}/apex-pipeline-gateway:${SHA_TAG}",
    "${REGISTRY}/apex-pipeline-gateway:latest",
  ]
}

// ── Auth Service ──────────────────────────────────
target "auth-svc" {
  inherits = ["service-base"]
  args = {
    CMAKE_TARGET = "auth_svc_main"
    SERVICE_DIR  = "auth-svc"
    CONFIG_FILE  = "auth_svc_e2e.toml"
    CI_IMAGE_TAG = CI_IMAGE_TAG
  }
  tags = [
    "${REGISTRY}/apex-pipeline-auth-svc:${SHA_TAG}",
    "${REGISTRY}/apex-pipeline-auth-svc:latest",
  ]
}

// ── Chat Service ──────────────────────────────────
target "chat-svc" {
  inherits = ["service-base"]
  args = {
    CMAKE_TARGET = "chat_svc_main"
    SERVICE_DIR  = "chat-svc"
    CONFIG_FILE  = "chat_svc_e2e.toml"
    CI_IMAGE_TAG = CI_IMAGE_TAG
  }
  tags = [
    "${REGISTRY}/apex-pipeline-chat-svc:${SHA_TAG}",
    "${REGISTRY}/apex-pipeline-chat-svc:latest",
  ]
}
```

> **실행**: `cd apex_infra && docker buildx bake -f docker/docker-bake.hcl services`

- [ ] **Step 2: 커밋**

```bash
git add apex_infra/docker/docker-bake.hcl
git commit -m "feat(infra): docker-bake.hcl — 서비스 Bake 타겟 매트릭스 (BACKLOG-176)"
git push
```

---

## Task 6: docker-compose.e2e.yml + valgrind.yml 업데이트

**Files:**
- Modify: `apex_infra/docker/docker-compose.e2e.yml:156-220`
- Modify: `apex_infra/docker/docker-compose.valgrind.yml`

- [ ] **Step 1: docker-compose.e2e.yml 서비스 빌드 블록 변경**

3개 서비스의 `build:` 블록을 통합 service.Dockerfile + 적절한 args로 변경. RSA 키는 volume mount로 전환:

gateway (156-183행):
```yaml
  gateway:
    build:
      context: ../..
      dockerfile: apex_infra/docker/service.Dockerfile
      target: runtime
      args:
        CI_IMAGE_TAG: ${CI_IMAGE_TAG:?CI_IMAGE_TAG is required}
        CMAKE_TARGET: apex_gateway
        SERVICE_DIR: gateway
        CONFIG_FILE: gateway_e2e.toml
    container_name: apex-e2e-gateway
    ports:
      - "8443:8443"
      - "8444:8444"
    volumes:
      - ../../apex_services/tests/keys/:/app/keys/:ro
    depends_on:
      kafka:
        condition: service_healthy
      redis-auth:
        condition: service_healthy
      redis-ratelimit:
        condition: service_healthy
      redis-pubsub:
        condition: service_healthy
    networks:
      - apex-e2e-net
    healthcheck:
      test: ["CMD-SHELL", "nc -z localhost 8443 || exit 1"]
      interval: 5s
      timeout: 3s
      retries: 10
      start_period: 10s
```

auth-svc (185-201행):
```yaml
  auth-svc:
    build:
      context: ../..
      dockerfile: apex_infra/docker/service.Dockerfile
      target: runtime
      args:
        CI_IMAGE_TAG: ${CI_IMAGE_TAG:?CI_IMAGE_TAG is required}
        CMAKE_TARGET: auth_svc_main
        SERVICE_DIR: auth-svc
        CONFIG_FILE: auth_svc_e2e.toml
    container_name: apex-e2e-auth-svc
    volumes:
      - ../../apex_services/tests/keys/:/app/keys/:ro
    depends_on:
      kafka:
        condition: service_healthy
      redis-auth:
        condition: service_healthy
      postgres:
        condition: service_healthy
    networks:
      - apex-e2e-net
```

chat-svc (203-220행) — 키 volume 불필요 (기존 Dockerfile에서도 키를 COPY하지 않음):
```yaml
  chat-svc:
    build:
      context: ../..
      dockerfile: apex_infra/docker/service.Dockerfile
      target: runtime
      args:
        CI_IMAGE_TAG: ${CI_IMAGE_TAG:?CI_IMAGE_TAG is required}
        CMAKE_TARGET: chat_svc_main
        SERVICE_DIR: chat-svc
        CONFIG_FILE: chat_svc_e2e.toml
    container_name: apex-e2e-chat-svc
    depends_on:
      kafka:
        condition: service_healthy
      redis-chat:
        condition: service_healthy
      redis-pubsub:
        condition: service_healthy
      postgres:
        condition: service_healthy
    networks:
      - apex-e2e-net
```

- [ ] **Step 2: docker-compose.valgrind.yml 업데이트**

서비스별 `build:` 블록에 필수 args 추가 (service.Dockerfile이 ARG를 필요로 하므로):

```yaml
services:
  gateway:
    build:
      target: valgrind
      args:
        CMAKE_TARGET: apex_gateway
        SERVICE_DIR: gateway
        CONFIG_FILE: gateway_e2e.toml
    environment:
      - APEX_REQUEST_TIMEOUT_MS=60000
    healthcheck:
      interval: 15s
      timeout: 30s
      retries: 15
      start_period: 60s

  auth-svc:
    build:
      target: valgrind
      args:
        CMAKE_TARGET: auth_svc_main
        SERVICE_DIR: auth-svc
        CONFIG_FILE: auth_svc_e2e.toml

  chat-svc:
    build:
      target: valgrind
      args:
        CMAKE_TARGET: chat_svc_main
        SERVICE_DIR: chat-svc
        CONFIG_FILE: chat_svc_e2e.toml

  kafka:
    healthcheck:
      interval: 15s
      timeout: 10s
      retries: 15
```

- [ ] **Step 3: 커밋**

```bash
git add apex_infra/docker/docker-compose.e2e.yml apex_infra/docker/docker-compose.valgrind.yml
git commit -m "refactor(infra): compose — 통합 service.Dockerfile 참조 + RSA 키 volume mount (BACKLOG-176)"
git push
```

---

## Task 7: docker-images.yml 워크플로우 생성

**Files:**
- Create: `.github/workflows/docker-images.yml`

- [ ] **Step 1: docker-images.yml 작성**

기존 docker-build.yml의 패턴을 기반으로, 2단계 빌드 + 태그 자동 갱신:

```yaml
name: Docker Images

permissions: {}

on:
  push:
    branches: [main]
    paths:
      - 'apex_infra/docker/tools-base.Dockerfile'
      - 'apex_infra/docker/ci.Dockerfile'
      - '**/vcpkg.json'
  workflow_call:   # ci.yml에서 호출 가능
  workflow_dispatch:
    inputs:
      force-tools:
        description: 'tools-base 강제 리빌드'
        type: boolean
        default: false

env:
  FORCE_JAVASCRIPT_ACTIONS_TO_NODE24: true
  REGISTRY: ghcr.io/gazuua

jobs:
  # ── Path filter ──────────────────────────────────
  changes:
    permissions:
      contents: read
    runs-on: ubuntu-latest
    outputs:
      tools: ${{ steps.filter.outputs.tools }}
      ci: ${{ steps.filter.outputs.ci }}
      tools_tag: ${{ steps.read-tag.outputs.tools_tag }}
    steps:
      - uses: actions/checkout@v4
      - name: Read tools image tag
        id: read-tag
        run: echo "tools_tag=$(cat .github/tools-image-tag | tr -d '[:space:]')" >> "$GITHUB_OUTPUT"
      - uses: dorny/paths-filter@v3
        id: filter
        with:
          filters: |
            tools:
              - 'apex_infra/docker/tools-base.Dockerfile'
            ci:
              - 'apex_infra/docker/ci.Dockerfile'
              - '**/vcpkg.json'

  # ── tools-base image ─────────────────────────────
  tools-base:
    needs: changes
    if: >-
      needs.changes.outputs.tools == 'true' ||
      github.event.inputs.force-tools == 'true'
    permissions:
      contents: write
      packages: write
    runs-on: ubuntu-latest
    timeout-minutes: 15
    outputs:
      tag: ${{ steps.tag.outputs.tag }}
    steps:
      - uses: actions/checkout@v4
      - uses: docker/login-action@v3
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}
      - uses: docker/build-push-action@v6
        with:
          context: .
          file: apex_infra/docker/tools-base.Dockerfile
          push: true
          tags: |
            ${{ env.REGISTRY }}/apex-pipeline-tools:latest
            ${{ env.REGISTRY }}/apex-pipeline-tools:sha-${{ github.sha }}
          cache-from: type=registry,ref=${{ env.REGISTRY }}/apex-pipeline-tools:latest
          cache-to: type=inline
      - name: Output tag
        id: tag
        run: echo "tag=sha-${{ github.sha }}" >> "$GITHUB_OUTPUT"
      - name: Update tools-image-tag
        if: github.ref == 'refs/heads/main'
        run: |
          echo "sha-${{ github.sha }}" > .github/tools-image-tag
          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git add .github/tools-image-tag
          git diff --staged --quiet || git commit -m "chore(ci): update tools-image-tag to sha-${{ github.sha }} [skip ci]"
          git pull --rebase origin main
          git push

  # ── CI image ─────────────────────────────────────
  ci:
    needs: [changes, tools-base]
    if: >-
      always() &&
      (needs.changes.outputs.ci == 'true' || needs.tools-base.result == 'success') &&
      (needs.tools-base.result == 'success' || needs.tools-base.result == 'skipped')
    permissions:
      contents: write
      packages: write
    runs-on: ubuntu-latest
    timeout-minutes: 45
    steps:
      - uses: actions/checkout@v4
      - uses: docker/login-action@v3
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}
      - name: Determine tools tag
        id: tools-tag
        run: |
          if [ "${{ needs.tools-base.outputs.tag }}" != "" ]; then
            echo "tag=${{ needs.tools-base.outputs.tag }}" >> "$GITHUB_OUTPUT"
          else
            echo "tag=$(cat .github/tools-image-tag | tr -d '[:space:]')" >> "$GITHUB_OUTPUT"
          fi
      - uses: docker/build-push-action@v6
        with:
          context: .
          file: apex_infra/docker/ci.Dockerfile
          push: true
          build-args: TOOLS_TAG=${{ steps.tools-tag.outputs.tag }}
          tags: |
            ${{ env.REGISTRY }}/apex-pipeline-ci:latest
            ${{ env.REGISTRY }}/apex-pipeline-ci:sha-${{ github.sha }}
          cache-from: type=registry,ref=${{ env.REGISTRY }}/apex-pipeline-ci:latest
          cache-to: type=inline
      - name: Update ci-image-tag
        if: github.ref == 'refs/heads/main'
        run: |
          echo "sha-${{ github.sha }}" > .github/ci-image-tag
          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git add .github/ci-image-tag
          git diff --staged --quiet || git commit -m "chore(ci): update ci-image-tag to sha-${{ github.sha }} [skip ci]"
          git pull --rebase origin main
          git push
```

- [ ] **Step 2: 커밋**

```bash
git add .github/workflows/docker-images.yml
git commit -m "feat(ci): docker-images.yml — tools-base+ci 2단계 이미지 빌드 워크플로우 (BACKLOG-176)"
git push
```

---

## Task 8: ci.yml 업데이트 (MSVC + Bake + Trivy)

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: changes 잡에 tools-image-tag 경로 추가**

`paths-ignore`에 `.github/tools-image-tag` 추가:
```yaml
  push:
    paths-ignore:
      # ... 기존 ...
      - '.github/tools-image-tag'
  pull_request:
    paths-ignore:
      # ... 기존 ...
      - '.github/tools-image-tag'
```

changes 잡의 docker 필터에 `tools-base.Dockerfile` 추가:
```yaml
            docker:
              - 'apex_infra/docker/ci.Dockerfile'
              - 'apex_infra/docker/tools-base.Dockerfile'
              - '**/vcpkg.json'
```

- [ ] **Step 2: build-image 잡을 docker-images.yml 참조로 변경**

```yaml
  build-image:
    needs: changes
    if: needs.changes.outputs.docker == 'true'
    uses: ./.github/workflows/docker-images.yml
    permissions:
      contents: write
      packages: write
```

- [ ] **Step 3: build-msvc 잡 추가**

build 잡 뒤에 추가:

```yaml
  # ── MSVC build & test ────────────────────────────
  build-msvc:
    permissions:
      contents: read
    needs: [changes]
    if: >-
      needs.changes.outputs.source == 'true' &&
      !contains(github.event.head_commit.message, '[skip-msvc]')
    name: msvc-release
    runs-on: windows-latest
    timeout-minutes: 30
    continue-on-error: true
    env:
      VCPKG_ROOT: C:\vcpkg

    steps:
      - uses: actions/checkout@v4

      - name: Setup sccache
        uses: mozilla-actions/sccache-action@v0.0.6

      - name: Restore vcpkg cache
        uses: actions/cache@v4
        with:
          path: vcpkg_installed
          key: vcpkg-msvc-${{ hashFiles('**/vcpkg.json') }}
          restore-keys: vcpkg-msvc-

      - name: Configure
        run: cmake --preset release-sccache
        env:
          SCCACHE_GHA_ENABLED: "true"

      - name: Build
        run: cmake --build build/Windows/release-sccache

      - name: Test
        run: ctest --preset release-sccache -LE e2e
```

- [ ] **Step 4: bake-services 잡 추가**

```yaml
  # ── Docker Bake (서비스 이미지 빌드) ─────────────
  bake-services:
    permissions:
      contents: read
      packages: write
    needs: [changes, build]
    if: >-
      always() &&
      needs.build.result == 'success' &&
      github.event_name == 'push' && github.ref == 'refs/heads/main'
    runs-on: ubuntu-latest
    timeout-minutes: 30
    steps:
      - uses: actions/checkout@v4

      - uses: docker/login-action@v3
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}

      - uses: docker/setup-buildx-action@v3

      - name: Bake and push service images
        working-directory: apex_infra
        env:
          CI_IMAGE_TAG: ${{ needs.changes.outputs.ci_image_tag }}
          SHA_TAG: sha-${{ github.sha }}
        run: |
          docker buildx bake -f docker/docker-bake.hcl services --push
```

- [ ] **Step 5: scan 잡 추가**

```yaml
  # ── Trivy 보안 스캔 ──────────────────────────────
  scan:
    permissions:
      contents: read
      security-events: write
    needs: [bake-services]
    if: needs.bake-services.result == 'success'
    runs-on: ubuntu-latest
    timeout-minutes: 10
    strategy:
      matrix:
        service: [gateway, auth-svc, chat-svc]
    steps:
      - uses: aquasecurity/trivy-action@master
        with:
          image-ref: ghcr.io/gazuua/apex-pipeline-${{ matrix.service }}:sha-${{ github.sha }}
          severity: CRITICAL,HIGH
          exit-code: 1
```

- [ ] **Step 6: 커밋**

```bash
git add .github/workflows/ci.yml
git commit -m "feat(ci): MSVC 빌드 잡 + Docker Bake + Trivy 보안 스캔 추가 (BACKLOG-176)"
git push
```

---

## Task 9: 기존 파일 삭제 + docker-build.yml 제거

**Files:**
- Delete: `apex_infra/docker/gateway.Dockerfile`
- Delete: `apex_infra/docker/auth-svc.Dockerfile`
- Delete: `apex_infra/docker/chat-svc.Dockerfile`
- Delete: `.github/workflows/docker-build.yml`

- [ ] **Step 1: 개별 서비스 Dockerfile 삭제**

```bash
git rm apex_infra/docker/gateway.Dockerfile
git rm apex_infra/docker/auth-svc.Dockerfile
git rm apex_infra/docker/chat-svc.Dockerfile
```

- [ ] **Step 2: docker-build.yml 삭제**

```bash
git rm .github/workflows/docker-build.yml
```

- [ ] **Step 3: 커밋**

```bash
git commit -m "refactor(infra): 개별 서비스 Dockerfile + docker-build.yml 삭제 — 통합 완료 (BACKLOG-176)"
git push
```

---

## Task 10: 문서 갱신

**Files:**
- Modify: `CLAUDE.md` (로드맵)
- Modify: `docs/Apex_Pipeline.md` (로드맵 + Docker 섹션)
- Create: `docs/docker-multistage-build/progress/YYYYMMDD_HHMMSS_docker_multistage_build.md`

- [ ] **Step 1: CLAUDE.md 로드맵 갱신**

현재 버전을 v0.6.2.0으로 갱신, 도구 섹션에 Docker Bake + sccache 언급 추가.

- [ ] **Step 2: docs/Apex_Pipeline.md 갱신**

§10 로드맵에서 v0.6.2.0 항목을 완료 상태로 기재. Docker 멀티스테이지 빌드 섹션에 tools-base/ci 2단계 구조, Bake, sccache 설명 추가.

- [ ] **Step 3: progress 문서 작성**

작업 결과 요약: 변경 파일, 핵심 결정, 예상 효과.

- [ ] **Step 4: backlog export**

```bash
apex-agent backlog export
```

- [ ] **Step 5: 커밋**

```bash
git add CLAUDE.md docs/Apex_Pipeline.md docs/docker-multistage-build/progress/
git commit -m "docs(infra): v0.6.2 Docker 멀티스테이지 빌드 문서 갱신 (BACKLOG-176)"
git push
```
