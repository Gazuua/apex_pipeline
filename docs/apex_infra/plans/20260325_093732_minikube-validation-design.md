# v0.6.3 Helm 차트 minikube 실배포 검증 — 설계서

> BACKLOG-205 | 브랜치: feature/helm-minikube-validation

## 1. 개요

v0.6.3에서 생성한 Helm 차트(PR #147)가 실제 K8s 환경(minikube)에서 한 번도 배포 테스트되지 않았다. 본 작업은 인프라 레이어를 중심으로 실배포 검증을 수행하여, Bitnami sub-chart 호환성, namespace 분리, Secret 주입, TOML ConfigMap 마운트 등 실배포에서만 드러나는 이슈를 사전에 식별하고 수정한다.

## 2. 검증 범위

### In-Scope
- **인프라 컴포넌트**: Kafka (KRaft), Redis x4 (alias), PostgreSQL, PgBouncer
- **Helm 메커니즘**: dependency update, template 렌더링, 2-release 전략, namespace 자동 생성
- **서비스 YAML 정합성**: `helm template`으로 렌더링 검증 (실배포 아님)
- **자동화 스크립트**: local-setup.sh, local-teardown.sh

### Out-of-Scope
- **앱 서비스 실배포**: Docker 이미지가 ghcr.io에 미존재 (BACKLOG-178에서 빌드 파이프라인 구축 예정)
- **서비스 간 Kafka 통신**: 앱 서비스 미배포로 불가
- **Ingress WebSocket 접속**: Gateway 미배포로 불가
- **Prometheus ServiceMonitor**: CRD 미설치 환경에서 스킵

## 3. 환경

| 도구 | 버전 |
|------|------|
| minikube | v1.38.1 |
| helm | v4.1.3 |
| kubectl | v1.34.1 |
| docker | 29.3.0 |
| OS | Windows 10 Pro (Docker Desktop 드라이버) |

minikube 설정: `--memory=4096 --cpus=2 --driver=docker`

## 4. 검증 전략

### Phase 1: 단계별 수동 검증

| 단계 | 검증 항목 | 성공 기준 |
|------|-----------|-----------|
| S1 | minikube 시작 + ingress addon | `minikube status` = Running |
| S2 | `helm dependency update` | Bitnami tgz 다운로드, Chart.lock 생성 |
| S3 | `helm template` 렌더링 (인프라) | 에러 0건, YAML 정합성 |
| S4 | `helm template` 렌더링 (서비스) | 에러 0건, ConfigMap/Secret/Deployment YAML 정합성 |
| S5 | Release 1 설치 (apex-infra) | 전체 인프라 Pod Ready |
| S6 | 인프라 상호 연결 확인 | PgBouncer→PostgreSQL, Redis 인증+ping |
| S7 | `helm test` (인프라) | 테스트 통과 |
| S8 | teardown | 깨끗한 정리 |

### Phase 2: 원클릭 스크립트 검증

| 단계 | 검증 항목 | 성공 기준 |
|------|-----------|-----------|
| S9 | `local-setup.sh` 실행 | 인프라 release 정상 설치 |
| S10 | `local-teardown.sh` 실행 | 리소스 전부 정리 |

## 5. 인프라 Pod 기대값

| 컴포넌트 | Pod 이름 패턴 | Ready | 추가 확인 |
|----------|--------------|-------|-----------|
| Kafka | `apex-infra-kafka-controller-0` | 1/1 | KRaft 부팅, 토픽 생성 가능 |
| Redis Auth | `apex-infra-redis-auth-master-0` | 1/1 | 인증 후 PONG |
| Redis Ratelimit | `apex-infra-redis-ratelimit-master-0` | 1/1 | 인증 후 PONG |
| Redis Chat | `apex-infra-redis-chat-master-0` | 1/1 | 인증 후 PONG |
| Redis Pubsub | `apex-infra-redis-pubsub-master-0` | 1/1 | 인증 후 PONG |
| PostgreSQL | `apex-infra-postgresql-0` | 1/1 | pg_isready, apex_user 존재 |
| PgBouncer | `apex-infra-pgbouncer-*` | 1/1 | PostgreSQL 연결 가능 |

## 6. local-setup.sh 예상 수정

- 서비스 이미지 미존재 상태에서 Release 2의 `--wait` 타임아웃 대응
- 이미지 사전 체크 또는 서비스 비활성화 옵션 추가 가능
- Phase 1 발견 이슈 반영

## 7. 이슈 처리 원칙

- 차트 렌더링 에러, Pod 기동 실패 → **즉시 수정 + 커밋**
- 스크립트 버그 → **즉시 수정 + 커밋**
- 서비스 이미지 빌드 파이프라인 → BACKLOG-178 (기존)
- 새 스코프 밖 이슈 → `backlog add`

## 8. 산출물

| 산출물 | 경로 |
|--------|------|
| 검증 결과 기록 | `docs/v0.6.3-k8s-helm/progress/YYYYMMDD_HHMMSS_minikube-validation.md` |
| 차트/스크립트 수정 | 이슈 발견 시 커밋 |

## 9. 성공 기준

1. `helm dependency update` 성공
2. `helm template` 렌더링 에러 0건 (인프라 + 서비스)
3. 인프라 Pod 전부 Running/Ready
4. 인프라 상호 연결 정상
5. `helm test` 통과
6. `local-setup.sh` / `local-teardown.sh` 정상 동작
7. 발견 이슈 전부 수정 완료
