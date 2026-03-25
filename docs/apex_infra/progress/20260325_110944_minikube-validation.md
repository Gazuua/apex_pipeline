# v0.6.3 Helm 차트 minikube 실배포 검증 — 결과 기록

> BACKLOG-205, BACKLOG-204 | 브랜치: feature/helm-minikube-validation | PR: #158
> 실행일: 2026-03-25

## 환경

| 도구 | 버전 |
|------|------|
| minikube | v1.38.1 |
| helm | v4.1.3 |
| kubectl | v1.34.1 |
| docker | 29.3.0 (Docker Desktop, 1903MB) |

## Phase 1: 단계별 수동 검증

### S0: BACKLOG-204 — bake-services CI 확인

- **결과**: CI 잡 **failure** 확인 (PR #147 머지 시점)
- **원인**: `docker-bake.hcl`의 `context = ".."` → HCL 파일 위치(`apex_infra/docker/`) 기준으로 해석되어 `apex_infra/`(repo root 아님)를 가리킴 → `lstat ../docker: no such file or directory`
- **수정**: `context = "../.."` (HCL 파일 기준 repo root)
- **추가 수정**: CI workflow에 `workflow_dispatch` 트리거 추가 + bake-services에 `workflow_dispatch` 허용 조건 추가 — 수동 트리거로 소스 변경 없이도 이미지 빌드 가능

### S1: minikube 시작 + ingress addon — PASS

- Docker Desktop 메모리 1903MB 제한으로 `--memory=4096` 실패
- `--memory=1803`으로 조정하여 성공

### S2: imagePullSecret — SKIP

- 서비스 이미지가 ghcr.io에 미존재 (bake-services 실패 이력)
- bake-services 수정 + workflow_dispatch 머지 후 수동 트리거로 이미지 빌드 예정

### S3: helm dependency update — PASS

- Bitnami chart 다운로드 성공: kafka 32.4.3, redis 21.2.14 (x4), postgresql 16.7.27

### S4-S5: helm template 렌더링 — PASS (수정 후)

- 인프라 + 서비스 렌더링 에러 0건
- FQDN, expand_env 패턴 정합성 확인
- **발견 이슈 (CRITICAL)**: 서비스 sub-chart template에서 `{{- include "apex-common.*"` 패턴이 저작권 주석 줄바꿈을 삼켜서 `apiVersion`이 주석에 병합됨 → `helm install` 시 `apiVersion not set` 에러
- **수정**: `{{-` → `{{` (16개 파일 일괄 수정)

### S6-S7: Release 1 (인프라) 설치 + 상호 연결 — PASS (수정 후)

**발견 이슈 3건:**

| # | 심각도 | 이슈 | 수정 |
|---|--------|------|------|
| 1 | **CRITICAL** | Bitnami Docker Hub 이미지 삭제 — kafka, pgbouncer 태그 전무, redis/postgresql은 latest만 잔존 | `image.registry: public.ecr.aws` + `global.security.allowInsecureImages: true` |
| 2 | **MAJOR** | PgBouncer liveness probe `pg_isready` — Bitnami 이미지에 바이너리 미포함 | `exec` → `tcpSocket` 포트 체크 |
| 3 | **MINOR** | Bitnami `allowInsecureImages` 필요 — ECR 레지스트리를 비표준으로 간주 | `global.security.allowInsecureImages: true` |

**인프라 Pod 상태 (수정 후):**

| 컴포넌트 | 상태 | 연결 확인 |
|----------|------|-----------|
| Kafka (KRaft) | 1/1 Running | 부팅 성공 |
| Redis Auth | 1/1 Running | PONG |
| Redis Ratelimit | 1/1 Running | PONG |
| Redis Chat | 1/1 Running | PONG |
| Redis Pubsub | 1/1 Running | PONG |
| PostgreSQL | 1/1 Running | accepting connections |
| PgBouncer | 1/1 Running | `SELECT 1` via PostgreSQL 성공 |

### S8-S10: Release 2 (서비스) — PARTIAL (수정 후)

**발견 이슈 2건:**

| # | 심각도 | 이슈 | 수정 |
|---|--------|------|------|
| 4 | **CRITICAL** | `{{- include` 줄바꿈 병합 → `apiVersion not set` | `{{-` → `{{` (16개 파일) |
| 5 | **MAJOR** | 2-release namespace 소유권 충돌 — Release 1이 생성한 namespace를 Release 2가 재생성 시도 | 서비스 release에 `--set namespaces.create=false` |

- 리소스 생성 성공 (Deployment, Service, ConfigMap, Secret, Ingress)
- Pod: ImagePullBackOff (서비스 이미지 미존재 — bake-services 수정 후 해결 예정)

### S11-S12: helm test + teardown — PASS

- 인프라 release: TEST SUITE None (Bitnami chart에 test 미포함)
- teardown: release 삭제 + namespace 삭제 정상

## Phase 2: 원클릭 스크립트 검증

### S13: local-setup.sh — PASS

- minikube 시작 (메모리 자동 감지) ✅
- ingress addon ✅
- helm dependency update ✅
- 인프라 release deployed ✅
- 서비스 release: --wait 타임아웃 → WARNING 출력 후 계속 (설계 대로)
- helm test: 서비스 미기동으로 Failed (예상 대로)
- **스크립트 exit code 0** — 정상 동작

### S14: local-teardown.sh — PASS

- services release uninstall ✅
- infra release uninstall ✅
- namespace 삭제 ✅

## 수정 커밋 요약

| 커밋 | 내용 |
|------|------|
| `90c7b64` | docker-bake.hcl context 경로 수정 (BACKLOG-204) |
| `77fd3c4` | Bitnami image registry → AWS ECR Public (BACKLOG-216) |
| `f3e6976` | PgBouncer probe tcpSocket 변경 + BACKLOG-216 등록 |
| `05d8e29` | template {{- 줄바꿈 버그 + namespace 충돌 + 스크립트 개선 |
| `dc6d635` | CI workflow_dispatch + bake-services 수동 트리거 |
| `bb6be5a` | CI helm-validation 잡 추가 (minikube 풀 배포 자동 검증) |

## 검증 환경 제약 사항

- **ServiceMonitor CRD**: minikube에 Prometheus Operator 미설치 시 ServiceMonitor 리소스 생성 불가 — 로컬에서는 비활성화, 프로덕션에서는 CRD 사전 설치 필요
- **메모리**: Docker Desktop 1903MB로 인프라 7 Pod까지는 가능하지만 서비스 추가 시 불안정 — 권장 4GB 이상
