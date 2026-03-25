# CI/CD 파이프라인 가이드

**버전**: v0.6.4.0 | **최종 갱신**: 2026-03-26
**목적**: 이 문서 하나만 읽고 CI/CD 파이프라인의 전체 구조를 이해하고 설정을 변경할 수 있다.

---

## 목차

- [1. 파이프라인 전체 구조](#1-파이프라인-전체-구조)
- [2. 워크플로우 파일 맵](#2-워크플로우-파일-맵)
- [3. Path Filter — 무엇이 CI를 트리거하는가](#3-path-filter--무엇이-ci를-트리거하는가)
- [4. Docker 이미지 계층](#4-docker-이미지-계층)
- [5. 빌드 잡 — 5개 preset](#5-빌드-잡--5개-preset)
- [6. E2E 테스트](#6-e2e-테스트)
- [7. 서비스 이미지 빌드 (bake-services)](#7-서비스-이미지-빌드-bake-services)
- [8. 3단계 검증 파이프라인](#8-3단계-검증-파이프라인)
- [9. Argo Rollouts canary 배포](#9-argo-rollouts-canary-배포)
- [10. Nightly Valgrind](#10-nightly-valgrind)
- [11. 설정 변경 레시피](#11-설정-변경-레시피)
- [12. 트러블슈팅](#12-트러블슈팅)
- [13. 핵심 파일 경로 맵](#13-핵심-파일-경로-맵)

---

## 1. 파이프라인 전체 구조

### PR / main push

```
                                  ┌─ format-check
                                  │
  push ──→ changes (path filter) ─┼─ build-image (Docker 변경 시)
                                  │      ↓
                                  ├─ build ─────────────────────────────────┐
                                  │   ├─ linux-gcc (debug)                 │
                                  │   ├─ linux-asan                        │
                                  │   ├─ linux-tsan                        │
                                  │   ├─ linux-ubsan                       │
                                  │   └─ msvc-release (Windows)            │
                                  │                                        │
                                  │   e2e ◄────── (build 성공 후)          │
                                  │                                        │
                                  │   bake-services ◄────── (build 성공 후)│
                                  │     │  PR: 빌드만 (push 없음)          │
                                  │     │  main: 빌드 + GHCR 푸시          │
                                  │     │                                  │
                                  │     ├─ scan (Trivy) ........... main만 │
                                  │     ├─ helm-validation ........ main만 │
                                  │     ├─ smoke-test ............. main만 │
                                  │     └─ deploy (Argo Rollouts) . main만 │
                                  │                                        │
                                  └─ go-test (Go 코드 변경 시)             │
                                                                           │
                                  build-msvc ◄─────────────────────────────┘
```

### main만 실행 vs 모든 push

| 잡 | PR에서 실행 | main에서 실행 | 비고 |
|----|:-----------:|:------------:|------|
| changes | O | O | path filter |
| format-check | O | O | 소스 변경 시 |
| build (5 preset) | O | O | 소스 변경 시 |
| e2e | O | O | build 성공 후 |
| bake-services | O (빌드만) | O (빌드+푸시) | build 성공 후 |
| go-test | O | O | Go 변경 시 |
| scan (Trivy) | - | O | bake 성공 후 |
| helm-validation | - | O | bake 성공 후 |
| smoke-test | - | O | bake 성공 후 |
| deploy | - | O | smoke+helm 성공 후 |

---

## 2. 워크플로우 파일 맵

| 파일 | 트리거 | 역할 |
|------|--------|------|
| `.github/workflows/ci.yml` | push, PR, workflow_dispatch | 메인 파이프라인 (빌드, 테스트, 배포) |
| `.github/workflows/docker-images.yml` | ci.yml에서 workflow_call, 독립 push | CI/tools-base Docker 이미지 빌드 |
| `.github/workflows/nightly.yml` | schedule (UTC 매일), workflow_dispatch | Valgrind memcheck (unit + e2e) |

---

## 3. Path Filter — 무엇이 CI를 트리거하는가

`ci.yml` 최상위에 `paths-ignore`가 있고, `changes` 잡에서 세부 필터링:

### 워크플로우 레벨 무시 (CI 자체가 안 뜸)

```yaml
paths-ignore:
  - '**/*.md'        # 마크다운
  - 'docs/**'        # 문서
  - 'LICENSE'
  - '.gitignore'
  - '.github/ci-image-tag'
  - '.github/tools-image-tag'
```

### changes 잡 세부 필터

| 출력 | 트리거 파일 | 영향 잡 |
|------|------------|---------|
| `docker` | ci.Dockerfile, tools-base.Dockerfile, vcpkg.json | build-image |
| `source` | *.cpp/hpp/h, CMakeLists.txt, CMakePresets.json, vcpkg.json, .github/workflows/**, 빌드 스크립트 | format-check, build, e2e |
| `go` | apex_tools/apex-agent/**/*.go, go.mod, go.sum | go-test |

**설정 변경**: `ci.yml`의 `dorny/paths-filter` `filters` 블록 수정.

---

## 4. Docker 이미지 계층

```
ubuntu:24.04
    ↓
tools-base.Dockerfile
  → ghcr.io/gazuua/apex-pipeline-tools:sha-xxx
  (빌드 도구: GCC, Ninja, sccache, vcpkg)
    ↓
ci.Dockerfile
  → ghcr.io/gazuua/apex-pipeline-ci:sha-xxx
  (tools-base + vcpkg 의존성 사전 설치)
    ↓
service.Dockerfile
  → ghcr.io/gazuua/apex-pipeline-{gateway,auth-svc,chat-svc}:sha-xxx
  (CI 이미지에서 빌드 → ubuntu:24.04 runtime)
```

### 이미지 태그 추적

CI 이미지 태그는 파일로 추적 — 워크플로우가 자동 갱신:
- `.github/ci-image-tag` — 현재 CI 이미지 SHA 태그
- `.github/tools-image-tag` — 현재 tools-base 이미지 SHA 태그

**설정 변경**: 이미지를 강제 리빌드하려면 `docker-images.yml`을 `workflow_dispatch`로 수동 실행.

### 서비스 이미지 빌드 preset

| 용도 | CMAKE_PRESET | 최적화 | 디버그 심볼 |
|------|:------------:|:------:|:----------:|
| E2E / 스모크 테스트 | debug | 없음 | 있음 |
| 배포 (GHCR 푸시) | release | 있음 | 없음 |

**설정 변경**: `docker-bake.hcl`의 `CMAKE_PRESET` 변수 기본값, CI에서 `CMAKE_PRESET=release` 환경변수로 오버라이드.

---

## 5. 빌드 잡 — 5개 preset

| preset | 컴파일러 | 목적 | 검출 대상 |
|--------|----------|------|-----------|
| debug (linux-gcc) | GCC | 기본 빌드 + 유닛 테스트 | 컴파일 오류, 로직 버그 |
| asan | GCC | AddressSanitizer | use-after-free, buffer overflow, 메모리 누수 |
| tsan | GCC | ThreadSanitizer | 데이터 레이스, 동시성 버그 |
| ubsan | GCC | UndefinedBehaviorSanitizer | 정수 오버플로, 널 역참조, 정렬 위반 |
| msvc-release | MSVC | Windows 크로스 컴파일 | 플랫폼 호환성 |

모든 preset은 `-Werror` (`/WX` MSVC) — **경고가 곧 빌드 실패**.

**설정 변경**: `CMakePresets.json`에서 preset 추가/수정. `ci.yml`의 `build.strategy.matrix`에 새 항목 추가.

### 아티팩트

`linux-gcc` 빌드에서 서비스 바이너리 + E2E 테스트 바이너리를 아티팩트로 업로드:
- `build/Linux/debug/apex_services/{service}/{binary}`
- `build/Linux/debug/apex_services/tests/e2e/apex_e2e_tests`

→ e2e, smoke-test 잡에서 다운로드하여 사용.

---

## 6. E2E 테스트

**실행 환경**: 인프라만 Docker, 서비스는 호스트에서 직접 실행.

```
[CI Runner (ubuntu)]
  ├─ docker-compose.e2e.yml (인프라만)
  │    ├─ Kafka (9092)
  │    ├─ Redis × 4 (6380-6383)
  │    └─ PostgreSQL (5432)
  │
  ├─ 호스트에서 실행 (빌드 아티팩트)
  │    ├─ apex_gateway
  │    ├─ auth_svc_main
  │    └─ chat_svc_main
  │
  └─ apex_e2e_tests (GTest)
```

**검증 대상**: 비즈니스 로직, 서비스 간 통신, 프로토콜 정합성.

### 테스트 스위트

| 파일 | 범위 |
|------|------|
| e2e_auth_test.cpp | 회원가입, 로그인, JWT, 로그아웃, 리프레시 |
| e2e_chat_test.cpp | 룸 생성/참여, 메시지 전송, 브로드캐스트 |
| e2e_ratelimit_test.cpp | IP/유저/엔드포인트 레이트리밋 |
| e2e_timeout_test.cpp | 요청 타임아웃 |
| e2e_stress_*.cpp | 부하 테스트 (CI에서는 스킵) |

**설정 변경**: E2E config 파일 (`apex_services/tests/e2e/*.toml`), 테스트 필터 (`ci.yml`의 `gtest_filter`).

---

## 7. 서비스 이미지 빌드 (bake-services)

### docker-bake.hcl

3개 서비스를 병렬 빌드하는 정의 파일.

```hcl
group "services" {
  targets = ["gateway", "auth-svc", "chat-svc"]
}
```

### 변수

| 변수 | 용도 | CI 설정 위치 |
|------|------|-------------|
| `CI_IMAGE_TAG` | CI 이미지 태그 | `changes` 잡 출력 → env |
| `SHA_TAG` | 커밋 SHA 기반 이미지 태그 | `sha-${{ github.sha }}` |
| `IS_MAIN` | main 브랜치 여부 (latest 태깅) | `github.ref == 'refs/heads/main'` |
| `CMAKE_PRESET` | 빌드 preset (debug/release) | main: release, PR: debug |
| `REGISTRY` | 이미지 레지스트리 | `ghcr.io/gazuua` |

### 태깅 전략

| 브랜치 | 태그 | latest |
|--------|------|--------|
| PR / feature | `sha-<commit>` | X |
| main | `sha-<commit>` | O |

**설정 변경**: `docker-bake.hcl`의 target `tags` 블록. `notequal(IS_MAIN, "true")` 삼항 연산자로 조건부 태깅.

### PR vs main 동작

```yaml
# PR — 로컬 빌드만 (GHCR 푸시 없음, 캐시 쓰기 없음)
docker buildx bake ... --set '*.output=type=docker' --set '*.cache-to='

# main — GHCR 푸시 + 캐시 쓰기
docker buildx bake ... --push
```

---

## 8. 3단계 검증 파이프라인

```
┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐
│     e2e          │    │   smoke-test     │    │ helm-validation  │
│                  │    │                  │    │                  │
│ 호스트 바이너리  │    │ Docker 이미지    │    │ minikube 배포    │
│ + Docker 인프라  │    │ + Docker 인프라  │    │ + Helm 차트      │
│                  │    │                  │    │                  │
│ "코드가 맞는지"  │    │ "이미지가 맞는지"│    │ "배포가 맞는지"  │
└──────────────────┘    └──────────────────┘    └──────────────────┘
      검출 대상               검출 대상               검출 대상
  • 로직 버그            • Dockerfile 오류       • Helm 렌더링 오류
  • 새니타이저 이슈      • 런타임 .so 누락       • K8s 리소스 충돌
  • 프로토콜 불일치      • config 경로 불일치     • probe 설정 오류
  • 레이스 컨디션        • Docker 네트워크 이슈   • 배포 순서 문제
```

### 8-1. smoke-test (docker-compose)

bake-services로 빌드된 **GHCR 이미지**를 docker-compose로 기동.

- **compose 파일**: `apex_infra/docker/docker-compose.smoke.yml`
- **config**: `apex_services/tests/e2e/docker/*_docker.toml` (Docker 네트워크용 엔드포인트)
- **테스트**: 기존 E2E 바이너리의 서브셋 (`--gtest_filter="*Login*:*SendMessage*"`)
- **main에서만 실행** — PR에서는 이미지가 GHCR에 없음

**설정 변경**:
- 스모크 테스트 시나리오: `ci.yml`의 `gtest_filter` 패턴
- Docker config: `apex_services/tests/e2e/docker/*.toml`
- compose 설정: `apex_infra/docker/docker-compose.smoke.yml`

### 8-2. helm-validation (minikube)

minikube에서 Helm 차트로 실배포 검증.

```
minikube start
  → GHCR 이미지 pull + load
  → helm install apex-infra (인프라)
  → helm install apex-services (서비스)
  → pod ready 대기
  → helm test
```

**설정 변경**: `ci.yml`의 `helm-validation` 잡. Helm values 오버라이드는 `--set` 플래그.

---

## 9. Argo Rollouts canary 배포

### 아키텍처

```
[Helm 차트]
  rollouts.enabled: false  →  일반 Deployment 생성
  rollouts.enabled: true   →  Argo Rollouts Rollout CRD 생성
```

### Canary 단계

```
setWeight: 10%  ──→  pause (수동 승격 대기)
       ↓
setWeight: 50%  ──→  pause (수동 승격 대기)
       ↓
setWeight: 100% ──→  완료
```

### Helm 템플릿 구조

```
apex-common (_helpers.tpl)
  ├─ apex-common.deployment   ← rollouts.enabled=false 일 때
  ├─ apex-common.rollout      ← rollouts.enabled=true 일 때
  └─ apex-common.podTemplate  ← 양쪽이 공유하는 Pod spec
```

각 서비스 sub-chart:
- `deployment.yaml` — `{{- if not (and .Values.rollouts .Values.rollouts.enabled) }}`
- `rollout.yaml` — `{{ include "apex-common.rollout" . }}`

### CI deploy 잡

main 머지 후 minikube에서 Rollout CRD 검증:

```
minikube start
  → Argo Rollouts CRD 설치 (v1.7.2)
  → GHCR 이미지 load
  → helm install (rollouts.enabled=true)
  → Rollout 리소스 생성 확인
  → Pod ready 대기
```

**설정 변경**:
- canary steps: 각 서비스 `values.yaml`의 `rollouts.canary.steps`
- Argo Rollouts 버전: `ci.yml`의 CRD install URL
- rollouts 활성화: `--set gateway.rollouts.enabled=true`

### 수동 승격 (로컬/staging)

```bash
# canary 상태 확인
kubectl argo rollouts get rollout <name> -n apex-services

# 다음 단계로 승격
kubectl argo rollouts promote <name> -n apex-services

# 즉시 전체 롤백
kubectl argo rollouts abort <name> -n apex-services
```

---

## 10. Nightly Valgrind

매일 UTC 자정에 실행. Valgrind memcheck로 메모리 오류 검출.

| 테스트 | 설정 |
|--------|------|
| Unit tests | `ctest --preset debug -LE e2e` + Valgrind |
| E2E tests | 호스트 서비스 + Valgrind wrapper |

타이밍 의존 테스트와 스트레스 테스트는 필터링.

**설정 변경**: `.github/workflows/nightly.yml`. Valgrind 옵션, 제외 테스트 목록.

---

## 11. 설정 변경 레시피

### 새 서비스 이미지 추가

1. `docker-bake.hcl`에 target 추가 (inherits `service-base`)
2. `group "services"`에 target 이름 추가
3. `ci.yml` → scan 잡의 `matrix.service`에 추가
4. `ci.yml` → helm-validation/deploy 잡의 이미지 load/set에 추가

### 빌드 시간 단축

- **sccache**: CI 이미지에 내장, `--mount=type=cache` 레이어 캐시
- **buildx cache**: `cache-from/cache-to` registry 기반
- **path filter**: 소스 미변경 시 빌드 자동 스킵

### CI 이미지 강제 리빌드

```
GitHub → Actions → Docker Images → Run workflow → force-tools: true
```

### 새 sanitizer preset 추가

1. `CMakePresets.json`에 configurePreset + buildPreset + testPreset 추가
2. `ci.yml` → `build.strategy.matrix.include`에 항목 추가

### E2E 테스트 추가

1. `apex_services/tests/e2e/`에 cpp 파일 추가
2. `CMakeLists.txt`에 소스 등록
3. Docker 환경 테스트 시 `apex_services/tests/e2e/docker/` config도 함께 업데이트

### Helm Rollout canary steps 변경

```yaml
# 서비스별 values.yaml
rollouts:
  canary:
    steps:
      - setWeight: 10
      - pause: { duration: 60 }   # 60초 후 자동 승격
      - setWeight: 50
      - pause: {}                  # 수동 승격 대기
      - setWeight: 100
```

---

## 12. 트러블슈팅

### CI 빌드 실패 — 경고 오류

```
error: ... [-Werror=...]
```

**원인**: `-Werror` 정책. 모든 경고가 빌드 실패.
**해결**: 코드 수정으로 경고 제거. 불가피한 경우만 `cmake/ApexWarnings.cmake`에 비활성화 + 사유 주석.

### bake-services 실패 — release preset

debug에서는 통과하지만 release에서 실패하는 경우:
- GCC release 모드에서 추가 최적화 경고 발생 가능
- `(void)` 캐스트가 `warn_unused_result`를 억제하지 못함 → `[[maybe_unused]] auto n = func();` 패턴 사용

### vcpkg 다운로드 실패

```
vcpkg: error: ... HTTP 502
```

**원인**: GitHub CDN 간헐적 장애.
**해결**: `gh run rerun --failed`

### helm-validation Pod 기동 실패

```
CreateContainerConfigError / CrashLoopBackOff
```

**디버깅**:
```bash
kubectl get pods -n apex-services -o wide
kubectl describe pod <pod-name> -n apex-services
kubectl logs <pod-name> -n apex-services
```

일반적 원인: 이미지 태그 불일치, ConfigMap 경로 오류, Secret 미생성.

### scan (Trivy) 실패

CRITICAL/HIGH 취약점 발견 시 빌드 실패.
**해결**: 기반 이미지 업데이트 (`ubuntu:24.04` → 최신 패치) 또는 취약점이 해당 없는 경우 `.trivyignore` 등록.

---

## 13. 핵심 파일 경로 맵

| 파일 | 용도 |
|------|------|
| `.github/workflows/ci.yml` | 메인 CI 파이프라인 |
| `.github/workflows/docker-images.yml` | CI/tools Docker 이미지 빌드 |
| `.github/workflows/nightly.yml` | Nightly Valgrind |
| `.github/ci-image-tag` | 현재 CI 이미지 SHA 태그 |
| `.github/tools-image-tag` | 현재 tools-base 이미지 SHA 태그 |
| `.github/CLAUDE.md` | CI 트러블슈팅 가이드 |
| `apex_infra/docker/service.Dockerfile` | 서비스 이미지 빌드 |
| `apex_infra/docker/docker-bake.hcl` | 서비스 이미지 빌드 정의 |
| `apex_infra/docker/docker-compose.e2e.yml` | E2E 환경 (소스 빌드) |
| `apex_infra/docker/docker-compose.smoke.yml` | 스모크 환경 (pre-built) |
| `apex_infra/k8s/apex-pipeline/` | Helm umbrella chart |
| `apex_infra/k8s/apex-pipeline/CLAUDE.md` | Helm 차트 규칙 |
| `CMakePresets.json` | CMake 빌드 preset 정의 |
| `cmake/ApexWarnings.cmake` | 경고 정책 |
| `.dockerignore` | Docker 빌드 컨텍스트 필터 |
