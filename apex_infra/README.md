# apex_infra

인프라 및 배포 설정 — Docker, Helm, CI/CD 파이프라인.

## 디렉토리 구조

```
apex_infra/
├── docker/
│   ├── ci.Dockerfile             # CI 빌드 이미지
│   ├── tools-base.Dockerfile     # CI 기반 이미지 (빌드 도구 + sccache + vcpkg)
│   ├── service.Dockerfile        # 서비스 이미지 (debug/release)
│   ├── docker-bake.hcl           # 서비스 이미지 빌드 정의 (3개 서비스)
│   ├── docker-compose.e2e.yml    # E2E 테스트 환경 (인프라 + 서비스 소스 빌드)
│   ├── docker-compose.smoke.yml  # 스모크 테스트 환경 (인프라 + pre-built 이미지)
│   └── init-sql/                 # PostgreSQL 초기 스키마
├── k8s/
│   └── apex-pipeline/            # Helm umbrella chart
│       ├── charts/               # sub-charts (gateway, auth-svc, chat-svc, apex-common)
│       ├── templates/            # umbrella 리소스 (namespace, RSA secret)
│       ├── values.yaml           # 로컬 기본값
│       └── values-prod.yaml      # 프로덕션 오버라이드
└── README.md
```

## Docker 이미지

### 이미지 계층

```
tools-base (ubuntu:24.04 + build tools + sccache + vcpkg)
    ↓
ci.Dockerfile (tools-base + vcpkg 의존성 사전 설치)
    ↓
service.Dockerfile (CI 이미지 기반 개별 서비스 빌드)
    ├── runtime stage (ubuntu:24.04 + runtime libs)
    └── valgrind stage (runtime + valgrind)
```

### 서비스 이미지 빌드

```bash
# 전체 서비스 (gateway, auth-svc, chat-svc) — debug preset
docker buildx bake -f apex_infra/docker/docker-bake.hcl services

# release preset (배포용)
docker buildx bake -f apex_infra/docker/docker-bake.hcl services CMAKE_PRESET=release

# GHCR 푸시 (CI에서만)
docker buildx bake -f apex_infra/docker/docker-bake.hcl services --push \
  SHA_TAG=sha-abc1234 IS_MAIN=true
```

### 이미지 태깅 전략

| 브랜치 | 태그 |
|--------|------|
| feature/* | `sha-<commit>` |
| main | `sha-<commit>` + `latest` |

## Docker Compose 환경

### E2E 테스트 환경

인프라 + 서비스 소스 빌드. `build:` 블록으로 소스에서 이미지 빌드.

```bash
# 기동 (인프라 + 서비스 빌드)
docker compose -f apex_infra/docker/docker-compose.e2e.yml up -d --build --wait

# E2E 테스트 실행
./build/Linux/debug/bin/apex_e2e_tests

# 종료
docker compose -f apex_infra/docker/docker-compose.e2e.yml down -v
```

### 스모크 테스트 환경

인프라 + pre-built GHCR 이미지. CI에서 bake-services 후 이미지 유효성 검증용.

```bash
# 기동 (SHA_TAG 필수)
SHA_TAG=sha-abc1234 docker compose -f apex_infra/docker/docker-compose.smoke.yml up -d --wait

# 스모크 테스트 (E2E 서브셋)
apex_e2e_tests --gtest_filter="*Login*:*SendMessage*"

# 종료
docker compose -f apex_infra/docker/docker-compose.smoke.yml down -v
```

### 인프라 서비스 구성

| 서비스 | 이미지 | 포트 | 용도 |
|--------|--------|------|------|
| Kafka (KRaft) | apache/kafka:4.2.0 | 9092 | 메시징 |
| Redis × 4 | redis:7.4.2-alpine | 6380-6383 | auth, ratelimit, chat, pubsub |
| PostgreSQL | postgres:16.8-alpine | 5432 | RDBMS |

## Helm 차트

### 구조

2-release namespace 분리 모델:

```
apex-infra (namespace: apex-infra)
  ├── Kafka (Bitnami)
  ├── Redis × 4 (Bitnami)
  ├── PostgreSQL (Bitnami)
  └── PgBouncer (Bitnami)

apex-services (namespace: apex-services)
  ├── Gateway
  ├── Auth-svc
  ├── Chat-svc
  └── (Log-svc — 미구현)
```

### Argo Rollouts (canary 배포)

`rollouts.enabled: true`로 Deployment 대신 Rollout CRD 사용.

```yaml
# values 오버라이드
gateway:
  rollouts:
    enabled: true
    canary:
      steps:
        - setWeight: 10
        - pause: {}
        - setWeight: 50
        - pause: {}
        - setWeight: 100
```

### 배포

```bash
cd apex_infra/k8s/apex-pipeline

# 1. 의존성 갱신
helm dependency update .

# 2. 인프라 release
helm upgrade --install apex-infra . -n apex-infra --create-namespace \
  --set gateway.enabled=false --set auth-svc.enabled=false \
  --set chat-svc.enabled=false \
  --wait --timeout 5m

# 3. 서비스 release
helm upgrade --install apex-services . -n apex-services --create-namespace \
  --set kafka.enabled=false --set redis-auth.enabled=false \
  --set redis-ratelimit.enabled=false --set redis-chat.enabled=false \
  --set redis-pubsub.enabled=false --set postgresql.enabled=false \
  --set pgbouncer.enabled=false \
  --wait --timeout 5m
```

### 로컬 검증 (minikube)

```bash
# minikube 시작
minikube start --memory=4096 --cpus=2

# 이미지 로드 (GHCR pull 대신)
docker build -f apex_infra/docker/service.Dockerfile \
  --build-arg CMAKE_TARGET=apex_gateway --build-arg SERVICE_DIR=gateway \
  --build-arg CONFIG_FILE=gateway_e2e.toml -t apex-gateway:local .
minikube image load apex-gateway:local

# Helm 배포 (위 배포 절차와 동일)
# 이미지 태그 오버라이드: --set gateway.image.tag=local
```

### Argo Rollouts 로컬 설치

```bash
# CRD 설치
kubectl apply -f https://github.com/argoproj/argo-rollouts/releases/download/v1.7.2/install.yaml

# 대시보드 (선택)
kubectl argo rollouts dashboard &
# → http://localhost:3100

# canary 승격
kubectl argo rollouts promote <rollout-name> -n apex-services
```

### 차트 변경 후 검증

```bash
cd apex_infra/k8s/apex-pipeline

# 구문 검사
helm lint .
helm lint . -f values-prod.yaml

# 렌더링 확인
helm template apex-services . \
  --set kafka.enabled=false --set redis-auth.enabled=false \
  --set redis-ratelimit.enabled=false --set redis-chat.enabled=false \
  --set redis-pubsub.enabled=false --set postgresql.enabled=false \
  --set pgbouncer.enabled=false

# Rollout CRD 렌더링 확인
helm template apex-services . \
  --set kafka.enabled=false --set redis-auth.enabled=false \
  --set redis-ratelimit.enabled=false --set redis-chat.enabled=false \
  --set redis-pubsub.enabled=false --set postgresql.enabled=false \
  --set pgbouncer.enabled=false \
  --set gateway.rollouts.enabled=true
```

상세: `apex_infra/k8s/apex-pipeline/CLAUDE.md`

## CI/CD 파이프라인

### 3단계 검증

```
e2e (호스트 바이너리)      → 코드가 맞는지 (비즈니스 로직)
smoke-test (docker-compose) → 이미지가 맞는지 (컨테이너 환경)
helm-validation (minikube)  → 배포가 맞는지 (K8s 오케스트레이션)
```

### 파이프라인 흐름

```
PR push / main push
  ├─ format-check
  ├─ build (debug, asan, tsan, ubsan, msvc)
  │   ├─ e2e
  │   └─ bake-services (PR: 빌드만, main: 빌드+GHCR 푸시)
  │       ├─ scan (Trivy, main only)
  │       ├─ helm-validation (minikube, main only)
  │       ├─ smoke-test (docker-compose, main only)
  │       └─ deploy (Argo Rollouts canary, main only)
  └─ go-test
```

## 관련 가이드

| 영역 | 파일 |
|------|------|
| Helm 차트 규칙 + 배포 절차 | `apex_infra/k8s/apex-pipeline/CLAUDE.md` |
| E2E 테스트 실행 + 트러블슈팅 | `apex_services/tests/e2e/CLAUDE.md` |
| CI 트러블슈팅 | `.github/CLAUDE.md` |
| 프레임워크 서비스 개발 API | `docs/apex_core/apex_core_guide.md` |
| 프로젝트 전체 로드맵 | `docs/Apex_Pipeline.md` |
