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
| RSA 키 보안 | BuildKit secret mount | 이미지 레이어에 키 잔류 방지. BACKLOG-147 해결 |
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
- `tools-base`: `.github/tools-image-tag` 파일로 관리
- `ci`: 기존 `.github/ci-image-tag` 유지
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

**service.Dockerfile 구조**:

```dockerfile
ARG CI_IMAGE_TAG=latest
FROM ghcr.io/gazuua/apex-pipeline-ci:${CI_IMAGE_TAG} AS builder

ARG SERVICE_NAME
COPY . /src
WORKDIR /src/build
RUN --mount=type=cache,target=/root/.cache/sccache \
    --mount=type=cache,target=/src/build/_cmake_cache,sharing=locked \
    cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_CXX_COMPILER_LAUNCHER=sccache && \
    cmake --build . --target ${SERVICE_NAME}_main

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libpq5 libsasl2-2 libzstd1 libcurl4 netcat-openbsd && \
    rm -rf /var/lib/apt/lists/*
RUN groupadd -r apex && useradd -r -g apex apex
ARG SERVICE_NAME
COPY --from=builder /src/build/apex_services/${SERVICE_NAME}/${SERVICE_NAME}_main /app/${SERVICE_NAME}
COPY --from=builder /src/apex_services/${SERVICE_NAME}/config/ /app/config/
USER apex
WORKDIR /app
ENTRYPOINT ["/app/${SERVICE_NAME}"]

FROM runtime AS valgrind
USER root
RUN apt-get update && apt-get install -y --no-install-recommends valgrind && \
    rm -rf /var/lib/apt/lists/*
USER apex
```

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
  args     = { SERVICE_NAME = "gateway", CI_IMAGE_TAG = CI_IMAGE_TAG }
  tags     = ["${REGISTRY}/apex-pipeline-gateway:latest"]
}

target "auth-svc" {
  inherits = ["service-base"]
  args     = { SERVICE_NAME = "auth_svc", CI_IMAGE_TAG = CI_IMAGE_TAG }
  tags     = ["${REGISTRY}/apex-pipeline-auth-svc:latest"]
}

target "chat-svc" {
  inherits = ["service-base"]
  args     = { SERVICE_NAME = "chat_svc", CI_IMAGE_TAG = CI_IMAGE_TAG }
  tags     = ["${REGISTRY}/apex-pipeline-chat-svc:latest"]
}
```

**빌드 명령**:
- 전체 병렬: `docker buildx bake services`
- 개별: `docker buildx bake gateway`
- CI 푸시: `docker buildx bake services --push`

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
    --mount=type=cache,target=/src/build/_cmake_cache,sharing=locked \
    cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_CXX_COMPILER_LAUNCHER=sccache && \
    cmake --build . --target ${SERVICE_NAME}_main
```

**로컬 개발 연계**:

```jsonc
// CMakePresets.json cacheVariables 추가
{
  "name": "linux-release",
  "cacheVariables": {
    "CMAKE_CXX_COMPILER_LAUNCHER": "sccache"
  }
}
```

- Windows build.bat 프리셋에도 동일 적용
- sccache 미설치 시 CMake가 무시하므로 기존 환경 비파괴

**GitHub Actions 캐시 연동** (비-Docker 빌드 잡):

```yaml
- uses: actions/cache@v4
  with:
    path: /root/.cache/sccache
    key: sccache-${{ runner.os }}-${{ hashFiles('**/*.cpp', '**/*.hpp') }}
    restore-keys: sccache-${{ runner.os }}-
```

### 3.4 .dockerignore

```dockerignore
# apex_infra/.dockerignore
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

빌드에 필요한 소스만 Docker context로 전달하여 전송 시간 단축.

### 3.5 RSA 키 보안 (BACKLOG-147 연계)

테스트용 RSA 키가 런타임 이미지 레이어에 영구 포함되는 보안 문제 해결:

```dockerfile
# 변경: BuildKit secret mount로 빌드 시에만 주입
RUN --mount=type=secret,id=rsa_key,target=/run/secrets/rsa_key \
    cp /run/secrets/rsa_key /app/config/jwt_key.pem
```

- 런타임 이미지 레이어에 키 미잔류
- docker-bake.hcl에서 secret 경로를 타겟별 주입
- E2E 테스트에서는 docker-compose secrets 블록으로 마운트

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
    # → ghcr.io/gazuua/apex-pipeline-tools:{latest,sha-xxx}

  ci:
    needs: [tools-base]
    # tools-base 위에 vcpkg install
    # → ghcr.io/gazuua/apex-pipeline-ci:{latest,sha-xxx}
```

### 4.3 ci.yml 잡 구성

```yaml
jobs:
  format-check:       # 기존 유지

  build-linux:        # 기존 build 잡 개편
    container: ghcr.io/gazuua/apex-pipeline-ci:${CI_IMAGE_TAG}
    # cmake configure + build (sccache) + ctest
    # sccache 캐시: actions/cache로 영속화

  build-msvc:         # 신규
    runs-on: windows-latest
    if: "!contains(github.event.head_commit.message, '[skip-msvc]')"
    # vcpkg install (actions/cache) + cmake preset (sccache) + ctest
    # 초기: continue-on-error: true → 안정화 후 필수

  bake-services:      # 신규
    needs: [build-linux]
    # docker buildx bake services
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
- `apex_infra/.dockerignore` — Docker context 최적화
- `.github/tools-image-tag` — tools-base 이미지 태그 관리
- `.github/workflows/docker-images.yml` — 이미지 빌드 워크플로우

### 수정 파일
- `apex_infra/docker/ci.Dockerfile` — tools-base 기반으로 개편 + cache mount
- `.github/workflows/ci.yml` — MSVC 잡 + Bake 잡 + Trivy 잡 추가
- `CMakePresets.json` — sccache launcher 추가
- `build.bat` — sccache 프리셋 반영 (선택적)
- `apex_infra/docker/docker-compose.e2e.yml` — Bake 타겟 참조로 전환

### 삭제 파일
- `apex_infra/docker/gateway.Dockerfile` — service.Dockerfile로 통합
- `apex_infra/docker/auth-svc.Dockerfile` — service.Dockerfile로 통합
- `apex_infra/docker/chat-svc.Dockerfile` — service.Dockerfile로 통합
- `.github/workflows/docker-build.yml` — docker-images.yml로 대체

## 7. 의존성

- v0.6.1 (Prometheus) 완료 — `/metrics` 엔드포인트가 헬스체크에 활용됨 ✅
- BACKLOG-147 (RSA 키 보안) — 이 작업에서 해결
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
| RSA 키 노출 | 이미지 레이어 잔류 | secret mount로 제거 |
