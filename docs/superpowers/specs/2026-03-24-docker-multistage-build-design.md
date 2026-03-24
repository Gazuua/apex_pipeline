# v0.6.2 Docker 멀티스테이지 빌드 고도화 — 설계 스펙

**일자**: 2026-03-24
**상태**: 승인됨
**백로그**: BACKLOG-176
**범위**: CI 이미지 레이어 분리 + Docker Bake + sccache + 보안 강화 + MSVC CI 빌드

---

## 1. 배경 및 목적

v0.6.1.0에서 Prometheus 메트릭 노출이 완성되었고, v0.6 운영 인프라 두 번째 마일스톤으로 Docker 빌드 파이프라인을 고도화한다.

**현재 상태**: 멀티스테이지 빌드 기본 구조는 v0.5.8에서 도입 완료 (builder→runtime→valgrind 3단계, CI 이미지 ghcr.io 호스팅). 그러나 레이어 최적화 부재, Dockerfile DRY 위반, 컴파일 캐시 미사용, 보안 이슈가 남아있다.

**목표**:
- CI 빌드 시간 대폭 단축 (sccache + BuildKit cache mount)
- 서비스 Dockerfile DRY 통합 + Docker Bake 병렬 빌드
- CI 이미지 레이어 분리로 변경 빈도별 재빌드 격리
- RSA 키 보안 문제 해결 (BACKLOG-147)
- Trivy 이미지 보안 스캔 CI 단계 추가
- MSVC release 빌드 CI 도입 (sccache 기반)

**비목표** (v0.6.2 범위 밖):
- docker-compose.prod.yml (v0.6.3 K8s/Helm에서 다룸)
- Grafana 대시보드 프로비저닝
- 원격 sccache 캐시 서버 (S3/GCS) — 로컬/GitHub Actions 캐시로 충분

## 2. 설계 결정

| 결정 | 선택 | 근거 |
|------|------|------|
| CI 이미지 분리 | 2단계 (tools-base → ci) | 업계 표준 (Envoy, gRPC, LLVM). 3단계는 현재 규모에 과도 |
| 서비스 Dockerfile | Docker Bake (docker-bake.hcl) | 서비스 3개→N개 확장 대비. 병렬 빌드+캐시 일괄 관리. 서비스 적을 때 도입해야 전환 비용 최소 |
| 컴파일 캐시 | sccache (Mozilla) | GCC+MSVC 양쪽 first-class 지원. ccache는 MSVC 지원 미성숙. 향후 원격 캐시 확장 가능 |
| 캐시 전략 | BuildKit cache mount + actions/cache 병행 | Docker 내부 빌드 = cache mount, 비-Docker 빌드(MSVC) = actions/cache. 둘 다 sccache 디렉토리 공유 |
| 런타임 이미지 | ubuntu:24.04 유지 | v0.6 개발 단계에서 디버깅 편의 우선. distroless는 v1.0 이후 재검토 |
| RSA 키 보안 | 런타임 볼륨 마운트 | 이미지에서 키 완전 제거. E2E는 docker-compose volumes, 프로덕션은 K8s secrets |
| 보안 스캔 | Trivy | 업계 표준, GitHub Actions 공식 액션 지원, CRITICAL/HIGH만 차단 |
| MSVC CI 빌드 | sccache 기반 Windows 잡 추가 | 캐시 히트 시 ~3분으로 현실적. 초기 continue-on-error → 안정화 후 필수 전환 |
| docker-compose.prod.yml | v0.6.3으로 미룸 | K8s Helm이 프로덕션 배포 정답 → compose.prod는 과도기 산출물 |

## 3. 아키텍처

### 3.1 CI 이미지 레이어 구조

```
tools-base.Dockerfile (변경 빈도: 월 1회 이하)
└─ ubuntu:24.04
   ├─ gcc-14, g++-14, cmake, ninja, git, python3, autoconf, libtool
   ├─ vcpkg CLI (커밋 고정)
   └─ sccache 바이너리
   → ghcr.io/gazuua/apex-pipeline-tools:{latest,sha-xxx}

ci.Dockerfile (변경 빈도: vcpkg.json 변경 시)
└─ FROM ghcr.io/gazuua/apex-pipeline-tools:${TOOLS_TAG}
   └─ RUN --mount=type=cache vcpkg install (루트, core, shared 3개 manifest)
   → ghcr.io/gazuua/apex-pipeline-ci:{latest,sha-xxx}
```

**재빌드 격리**:

| 변경 유형 | 재빌드 대상 | 예상 시간 |
|-----------|------------|-----------|
| 도구 버전 업 (gcc, cmake) | tools-base만 | ~5분 |
| vcpkg.json 변경 | ci만 (tools 캐시 히트) | ~30분 |
| 소스 코드만 변경 | ci 이미지 재사용 (변경 없음) | 0분 |

**태그 관리**:
- `tools-base`: `.github/tools-image-tag` 파일로 관리 (docker-images.yml이 빌드 후 자동 갱신 — 기존 ci-image-tag 패턴과 동일)
- `ci`: 기존 `.github/ci-image-tag` 유지 (docker-images.yml로 이관)
- 둘 다 `latest` + `sha-{commit}` 듀얼 태그

### 3.2 Docker Bake + 서비스 Dockerfile 통합

**현행**: 3개 Dockerfile (94% 동일 코드)

**변경**: 단일 `service.Dockerfile` + `docker-bake.hcl`

```
apex_infra/docker/
├── tools-base.Dockerfile   (신규)
├── ci.Dockerfile            (개편)
├── service.Dockerfile       (통합 — 기존 3개 대체)
└── docker-bake.hcl          (신규)
```

**서비스별 파라미터 매핑** (현행 Dockerfile에서 추출):

| 파라미터 | gateway | auth-svc | chat-svc |
|----------|---------|----------|----------|
| `CMAKE_TARGET` | `apex_gateway` | `auth_svc_main` | `chat_svc_main` |
| `SERVICE_DIR` | `gateway` | `auth-svc` | `chat-svc` |
| `CONFIG_FILE` | `gateway_e2e.toml` | `auth_svc_e2e.toml` | `chat_svc_e2e.toml` |

> **Note**: gateway의 CMake 타겟명은 `apex_gateway`로, auth/chat의 `*_main` 패턴과 다르다. 통합 Dockerfile은 `CMAKE_TARGET` ARG로 이 차이를 흡수한다.

**service.Dockerfile 구조**:

```dockerfile
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

> **ENTRYPOINT 주의**: Docker exec form (`["/app/..."]`)은 변수 치환을 지원하지 않으므로, `ENV APP_BINARY`를 설정한 뒤 shell form으로 `exec`하여 PID 1 시그널 처리를 유지한다.

**docker-bake.hcl 구조**:

```hcl
variable "CI_IMAGE_TAG" { default = "latest" }
variable "REGISTRY"     { default = "ghcr.io/gazuua" }

group "services" {
  targets = ["gateway", "auth-svc", "chat-svc"]
}

target "service-base" {
  dockerfile = "docker/service.Dockerfile"
  context    = ".."
  cache-from = ["type=registry,ref=${REGISTRY}/apex-pipeline-cache:buildcache"]
  cache-to   = ["type=registry,ref=${REGISTRY}/apex-pipeline-cache:buildcache,mode=max"]
}

target "gateway" {
  inherits = ["service-base"]
  args = {
    CMAKE_TARGET = "apex_gateway"
    SERVICE_DIR  = "gateway"
    CONFIG_FILE  = "gateway_e2e.toml"
    CI_IMAGE_TAG = CI_IMAGE_TAG
  }
  tags = ["${REGISTRY}/apex-pipeline-gateway:latest"]
}

target "auth-svc" {
  inherits = ["service-base"]
  args = {
    CMAKE_TARGET = "auth_svc_main"
    SERVICE_DIR  = "auth-svc"
    CONFIG_FILE  = "auth_svc_e2e.toml"
    CI_IMAGE_TAG = CI_IMAGE_TAG
  }
  tags = ["${REGISTRY}/apex-pipeline-auth-svc:latest"]
}

target "chat-svc" {
  inherits = ["service-base"]
  args = {
    CMAKE_TARGET = "chat_svc_main"
    SERVICE_DIR  = "chat-svc"
    CONFIG_FILE  = "chat_svc_e2e.toml"
    CI_IMAGE_TAG = CI_IMAGE_TAG
  }
  tags = ["${REGISTRY}/apex-pipeline-chat-svc:latest"]
}
```

**빌드 명령** (apex_infra/ 디렉토리에서 실행):
- 전체 병렬: `docker buildx bake -f docker/docker-bake.hcl services`
- 개별: `docker buildx bake -f docker/docker-bake.hcl gateway`
- CI 푸시: `docker buildx bake -f docker/docker-bake.hcl services --push`

> **context 해석**: `context = ".."` 는 HCL 파일 기준 상대경로. `docker buildx bake -f docker/docker-bake.hcl` 실행 시 context는 `apex_infra/..` = 레포 루트가 된다.

### 3.3 BuildKit Cache Mount + sccache

3종 캐시를 Docker 빌드 내에서 mount로 관리:

| 캐시 | 마운트 경로 | 무효화 조건 | 효과 |
|------|------------|------------|------|
| vcpkg binary cache | `/opt/vcpkg_cache` | vcpkg.json 변경 | 패키지 재설치 시간 ~30분→~3분 |
| sccache (컴파일) | `/root/.cache/sccache` | 소스 변경 | 풀빌드 ~3분→증분 ~15초 |
| CMake configure cache | `/src/build/_cmake_cache` | CMakeLists.txt 변경 | configure 스킵 |

**CI 이미지에서의 적용** (ci.Dockerfile):

```dockerfile
RUN --mount=type=cache,target=/opt/vcpkg_cache,sharing=locked \
    vcpkg install --x-install-root=/opt/vcpkg_installed \
                  --binarysource="files,/opt/vcpkg_cache,readwrite"
```

**서비스 빌드에서의 적용** (service.Dockerfile):

```dockerfile
RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake --preset debug -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed \
          -DCMAKE_CXX_COMPILER_LAUNCHER=sccache \
    && cmake --build build/Linux/debug --target ${CMAKE_TARGET}
```

**로컬 개발 연계**:

sccache는 별도 CMake 프리셋으로 opt-in 방식 적용. sccache 미설치 환경에서도 기존 빌드가 깨지지 않는다:

```jsonc
// CMakePresets.json — 기존 프리셋에 inherits로 확장
{
  "name": "debug-sccache",
  "inherits": "debug",
  "cacheVariables": {
    "CMAKE_CXX_COMPILER_LAUNCHER": "sccache"
  }
}
```

> **주의**: `CMAKE_CXX_COMPILER_LAUNCHER`에 지정된 바이너리가 PATH에 없으면 CMake configure가 실패한다. 따라서 기존 프리셋을 수정하지 않고 별도 `*-sccache` 프리셋을 추가하여 opt-in한다.

**GitHub Actions 캐시 연동** (비-Docker 빌드 잡):

```yaml
- uses: actions/cache@v4
  with:
    path: /root/.cache/sccache
    key: sccache-${{ runner.os }}-${{ hashFiles('**/*.cpp', '**/*.hpp') }}
    restore-keys: sccache-${{ runner.os }}-
```

### 3.4 .dockerignore

Docker Bake의 `context = ".."` (레포 루트)에 맞춰 **레포 루트**에 `.dockerignore`를 배치:

```dockerignore
# .dockerignore (레포 루트)
.git
build/
docs/
*.md
apex_tools/
apex_infra/prometheus/
apex_infra/grafana/
**/*.bat
**/__pycache__/
.claude/
.github/
```

빌드에 필요한 소스(`apex_core/`, `apex_shared/`, `apex_services/`, `CMakeLists.txt`, `vcpkg*.json`, `cmake/`, `CMakePresets.json`)만 Docker context로 전달하여 전송 시간 단축.

> **기존 .dockerignore**: 현재 레포 루트에 화이트리스트 패턴(`*` 후 `!` 예외)의 `.dockerignore`가 존재한다면 그 패턴을 유지하면서 최적화한다. 없으면 위 블랙리스트 패턴으로 신규 생성.

### 3.5 RSA 키 보안 (BACKLOG-147 연계)

테스트용 RSA 키(`apex_services/tests/keys/`)가 gateway, auth-svc 런타임 이미지에 `COPY`로 포함되는 보안 문제를 해결한다.

**원칙**: 키는 이미지에 절대 포함하지 않는다. 런타임에 마운트한다.

**E2E 테스트** (docker-compose volumes):
```yaml
# docker-compose.e2e.yml
services:
  gateway:
    volumes:
      - ../apex_services/tests/keys/:/app/keys/:ro
  auth-svc:
    volumes:
      - ../apex_services/tests/keys/:/app/keys/:ro
```

**프로덕션** (v0.6.3 K8s에서):
- K8s Secrets → Pod volume mount

**service.Dockerfile에서**: `COPY apex_services/tests/keys/` 라인을 제거하고, 키가 필요한 서비스는 docker-compose volumes 또는 K8s secrets로 주입.

### 3.6 보안 스캔 (Trivy)

CI에 이미지 취약점 스캔 단계 추가:

```yaml
scan:
  needs: [bake-services]
  runs-on: ubuntu-latest
  strategy:
    matrix:
      service: [gateway, auth-svc, chat-svc]
  steps:
    - uses: aquasecurity/trivy-action@master
      with:
        image-ref: ghcr.io/gazuua/apex-pipeline-${{ matrix.service }}:${{ github.sha }}
        severity: CRITICAL,HIGH
        exit-code: 1
```

- 서비스 이미지 Bake 후 3개 병렬 스캔
- CRITICAL/HIGH만 차단, MEDIUM 이하 경고만

## 4. CI 워크플로우 재설계

### 4.1 워크플로우 구조

```
현행:
  docker-build.yml  → ci.Dockerfile 빌드 + 푸시
  ci.yml            → format-check + (조건부 docker-build) + 빌드+테스트
  nightly.yml       → valgrind

변경:
  docker-images.yml → tools-base + ci 이미지 빌드 (2단계 분리)
  ci.yml            → format-check + Linux/GCC + MSVC + Bake + Trivy
  nightly.yml       → valgrind (기존 유지)
```

### 4.2 docker-images.yml (기존 docker-build.yml 대체)

```yaml
on:
  push:
    paths:
      - 'apex_infra/docker/tools-base.Dockerfile'
      - 'apex_infra/docker/ci.Dockerfile'
      - '**/vcpkg.json'
  workflow_dispatch:

jobs:
  tools-base:
    # tools-base.Dockerfile 변경 시만 빌드
    # 빌드 후 .github/tools-image-tag 자동 갱신 (SHA 기록)
    # → ghcr.io/gazuua/apex-pipeline-tools:{latest,sha-xxx}

  ci:
    needs: [tools-base]
    # tools-base 위에 vcpkg install
    # 빌드 후 .github/ci-image-tag 자동 갱신
    # → ghcr.io/gazuua/apex-pipeline-ci:{latest,sha-xxx}
```

### 4.3 ci.yml 잡 구성

```yaml
jobs:
  format-check:       # 기존 유지

  build-linux:        # 기존 build 잡 개편
    container: ghcr.io/gazuua/apex-pipeline-ci:${CI_IMAGE_TAG}
    # cmake --preset debug-sccache + build + ctest
    # sccache 캐시: actions/cache로 영속화

  build-msvc:         # 신규
    runs-on: windows-latest
    if: "!contains(github.event.head_commit.message, '[skip-msvc]')"
    # vcpkg install (actions/cache) + cmake --preset msvc-release-sccache + ctest
    # 초기: continue-on-error: true → 안정화 후 필수

  bake-services:      # 신규
    needs: [build-linux]
    # docker buildx bake -f apex_infra/docker/docker-bake.hcl services
    # PR: 빌드만 / main push: 빌드+푸시

  scan:               # 신규
    needs: [bake-services]
    # Trivy 3개 서비스 병렬 스캔
```

### 4.4 MSVC 빌드 전략

- **기본 활성**: 모든 PR/push에서 실행
- **스킵**: 커밋 메시지에 `[skip-msvc]` (문서 전용 PR 등)
- **sccache**: actions/cache로 `C:\Users\runneradmin\.cache\sccache` 영속화
- **vcpkg**: actions/cache로 바이너리 캐시 영속화
- **프리셋**: `msvc-release-sccache` (기존 `msvc-release` inherits + sccache launcher)
- **실패 정책**: 초기 `continue-on-error: true` (경고) → 안정화 후 필수 전환
- **캐시 히트 시 예상**: 풀빌드 ~12분 → 증분 ~3분

## 5. 이미지 태깅 전략

```
ghcr.io/gazuua/apex-pipeline-tools:{latest,sha-xxx}
ghcr.io/gazuua/apex-pipeline-ci:{latest,sha-xxx}
ghcr.io/gazuua/apex-pipeline-gateway:{latest,sha-xxx,vX.Y.Z}
ghcr.io/gazuua/apex-pipeline-auth-svc:{latest,sha-xxx,vX.Y.Z}
ghcr.io/gazuua/apex-pipeline-chat-svc:{latest,sha-xxx,vX.Y.Z}
```

- `sha-{short}`: 모든 빌드에 부여 (추적성)
- `latest`: main 머지 시에만 갱신
- `v{semver}`: 릴리스 태그 시 부여

## 6. 파일 변경 목록

### 신규 파일
- `apex_infra/docker/tools-base.Dockerfile` — 도구 베이스 이미지
- `apex_infra/docker/service.Dockerfile` — 통합 서비스 Dockerfile
- `apex_infra/docker/docker-bake.hcl` — Bake 타겟 정의
- `.dockerignore` — Docker context 최적화 (레포 루트)
- `.github/tools-image-tag` — tools-base 이미지 태그 관리
- `.github/workflows/docker-images.yml` — 이미지 빌드 워크플로우

### 수정 파일
- `apex_infra/docker/ci.Dockerfile` — tools-base 기반으로 개편 + cache mount
- `.github/workflows/ci.yml` — MSVC 잡 + Bake 잡 + Trivy 잡 추가
- `CMakePresets.json` — `debug-sccache`, `msvc-release-sccache` 프리셋 추가 (기존 프리셋 불변)
- `apex_infra/docker/docker-compose.e2e.yml` — Bake 이미지 참조 + RSA 키 volumes 마운트로 전환

### 삭제 파일
- `apex_infra/docker/gateway.Dockerfile` — service.Dockerfile로 통합
- `apex_infra/docker/auth-svc.Dockerfile` — service.Dockerfile로 통합
- `apex_infra/docker/chat-svc.Dockerfile` — service.Dockerfile로 통합
- `.github/workflows/docker-build.yml` — docker-images.yml로 대체

## 7. 의존성

- v0.6.1 (Prometheus) 완료 — `/metrics` 엔드포인트가 헬스체크에 활용됨 ✅
- BACKLOG-147 (RSA 키 보안) — 이 작업에서 해결 (이미지에서 제거 → 런타임 마운트)
- v0.6.3 (K8s/Helm) — 이 작업의 이미지 태깅/Bake 구조 위에 구축

## 8. 예상 효과 요약

| 지표 | 현행 | 변경 후 |
|------|------|---------|
| 소스 1파일 변경 시 CI 빌드 | ~3분 (풀빌드) | ~15초 (sccache 증분) |
| vcpkg.json 변경 시 이미지 | ~1시간 (전체) | ~30분 (ci만, tools 캐시 히트) |
| 서비스 Dockerfile 유지보수 | 3개 파일 | 1개 + Bake 타겟 |
| 서비스 추가 시 작업 | Dockerfile 복사+수정 | Bake 타겟 1블록 추가 |
| MSVC 빌드 검증 | 로컬만 | CI 자동 (캐시 히트 ~3분) |
| 보안 스캔 | 없음 | Trivy CRITICAL/HIGH 자동 차단 |
| RSA 키 노출 | 이미지 레이어 잔류 | 이미지에서 완전 제거, 런타임 마운트 |
