# v0.6.3 Helm 차트 minikube 실배포 검증 — 설계서

> BACKLOG-205, BACKLOG-204 | 브랜치: feature/helm-minikube-validation

## 1. 개요

v0.6.3에서 생성한 Helm 차트(PR #147)가 실제 K8s 환경(minikube)에서 한 번도 배포 테스트되지 않았다. 본 작업은 인프라 + 앱 서비스를 포함한 **풀 파이프라인 실배포 검증**을 수행한다.

CI `bake-services` 잡이 main push마다 서비스 이미지(gateway, auth-svc, chat-svc)를 `ghcr.io/gazuua/`에 빌드/push하고 있으므로, 서비스까지 포함한 전체 검증이 가능하다. BACKLOG-204(Docker Bake HCL 경로 수정 CI 검증)도 병행한다.

## 2. 검증 범위

### In-Scope
- **인프라 컴포넌트**: Kafka (KRaft), Redis x4 (alias), PostgreSQL, PgBouncer
- **앱 서비스**: Gateway, Auth-svc, Chat-svc (ghcr.io에서 이미지 pull)
- **Helm 메커니즘**: dependency update, template 렌더링, 2-release 전략, namespace 자동 생성
- **이미지 Pull**: ghcr.io private registry → minikube imagePullSecret 설정
- **서비스 간 연결**: 인프라 ↔ 서비스 FQDN 통신 (Kafka, Redis, PostgreSQL)
- **Ingress**: Gateway WebSocket Ingress 리소스 생성 및 접속 확인
- **자동화 스크립트**: local-setup.sh, local-teardown.sh
- **BACKLOG-204**: docker-bake.hcl 경로 수정 후 bake-services 잡 정상 동작 확인

### Out-of-Scope
- **log-svc**: 바이너리 미구현 (BACKLOG-199), `enabled: false` 기본
- **Prometheus ServiceMonitor**: CRD 미설치 환경에서 스킵 (리소스 생성은 template 렌더링으로 검증)
- **E2E 기능 테스트**: 실제 API 호출/채팅 시나리오는 별도 테스트 영역

## 3. 환경

| 도구 | 버전 |
|------|------|
| minikube | v1.38.1 |
| helm | v4.1.3 |
| kubectl | v1.34.1 |
| docker | 29.3.0 |
| OS | Windows 10 Pro (Docker Desktop 드라이버) |

minikube 설정: `--memory=4096 --cpus=2 --driver=docker` (OOM 시 6144로 조정)

## 4. 서비스 이미지 가용성

CI `bake-services` 잡 (`ci.yml:324`):
- 트리거: main push + build 성공 시
- 빌드: `docker-bake.hcl` → gateway, auth-svc, chat-svc
- Push: `ghcr.io/gazuua/apex-pipeline-{service}:latest` + `:sha-{commit}`
- **ghcr.io가 private이므로 minikube에서 imagePullSecret 필요**

BACKLOG-204 확인 사항:
- docker-bake.hcl의 dockerfile 경로가 정상인지
- 최근 main push에서 bake-services 잡이 실제로 통과했는지 (gh CLI로 확인)

## 5. 검증 전략

### Phase 1: 단계별 수동 검증

| 단계 | 검증 항목 | 성공 기준 |
|------|-----------|-----------|
| S0 | BACKLOG-204 확인 — bake-services CI 잡 통과 여부 | 최근 실행에서 success |
| S1 | minikube 시작 + ingress addon | `minikube status` = Running |
| S2 | ghcr.io imagePullSecret 생성 | Secret 존재, 인증 유효 |
| S3 | `helm dependency update` | Bitnami tgz 다운로드, Chart.lock 생성 |
| S4 | `helm template` 렌더링 (인프라) | 에러 0건 |
| S5 | `helm template` 렌더링 (서비스) | 에러 0건, FQDN/expand_env 정합성 |
| S6 | Release 1 설치 (apex-infra) | 전체 인프라 Pod Ready |
| S7 | 인프라 상호 연결 확인 | PgBouncer→PostgreSQL, Redis 인증+ping |
| S8 | Release 2 설치 (apex-services) | Gateway, Auth, Chat Pod Ready |
| S9 | 서비스 → 인프라 연결 확인 | 서비스 Pod 로그에 연결 성공 로그 |
| S10 | Ingress 접속 확인 | Gateway Ingress 리소스 생성, 라우팅 가능 |
| S11 | `helm test` | 테스트 통과 |
| S12 | teardown | 깨끗한 정리 |

### Phase 2: 원클릭 스크립트 검증

| 단계 | 검증 항목 | 성공 기준 |
|------|-----------|-----------|
| S13 | `local-setup.sh` 실행 | 인프라 + 서비스 release 정상 설치 |
| S14 | `local-teardown.sh` 실행 | 리소스 전부 정리 |

## 6. Pod 기대값

### 인프라 (apex-infra namespace)

| 컴포넌트 | Pod 이름 패턴 | Ready | 추가 확인 |
|----------|--------------|-------|-----------|
| Kafka | `apex-infra-kafka-controller-0` | 1/1 | KRaft 부팅 |
| Redis Auth | `apex-infra-redis-auth-master-0` | 1/1 | 인증 후 PONG |
| Redis Ratelimit | `apex-infra-redis-ratelimit-master-0` | 1/1 | 인증 후 PONG |
| Redis Chat | `apex-infra-redis-chat-master-0` | 1/1 | 인증 후 PONG |
| Redis Pubsub | `apex-infra-redis-pubsub-master-0` | 1/1 | 인증 후 PONG |
| PostgreSQL | `apex-infra-postgresql-0` | 1/1 | pg_isready |
| PgBouncer | `apex-infra-pgbouncer-*` | 1/1 | PG 연결 가능 |

### 서비스 (apex-services namespace)

| 컴포넌트 | Pod 이름 패턴 | Ready | 추가 확인 |
|----------|--------------|-------|-----------|
| Gateway | `apex-services-gateway-*` | 1/1 | 포트 8443 listen |
| Auth-svc | `apex-services-auth-svc-*` | 1/1 | Kafka/Redis/PG 연결 |
| Chat-svc | `apex-services-chat-svc-*` | 1/1 | Kafka/Redis/PG 연결 |

## 7. imagePullSecret 전략

ghcr.io private registry 접근을 위해:

```bash
kubectl create secret docker-registry ghcr-secret \
  --docker-server=ghcr.io \
  --docker-username=<github-user> \
  --docker-password=<PAT-or-GITHUB_TOKEN> \
  -n apex-services
```

서비스 sub-chart의 `imagePullSecrets` 설정 확인 필요. 없으면 차트 수정.

## 8. local-setup.sh 예상 수정

- imagePullSecret 자동 생성 로직 추가 (또는 사전 조건 문서화)
- Phase 1 발견 이슈 반영
- 서비스 Pod `--wait` 타임아웃 적정치 조정

## 9. 이슈 처리 원칙

- 차트 렌더링 에러, Pod 기동 실패 → **즉시 수정 + 커밋**
- 스크립트 버그 → **즉시 수정 + 커밋**
- bake-services CI 실패 → **BACKLOG-204로 즉시 수정**
- 새 스코프 밖 이슈 → `backlog add`

## 10. 산출물

| 산출물 | 경로 |
|--------|------|
| 검증 결과 기록 | `docs/apex_infra/progress/20260325_110944_minikube-validation.md` |
| 차트/스크립트 수정 | 이슈 발견 시 커밋 |

## 11. 성공 기준

1. BACKLOG-204 확인 — bake-services CI 잡 성공 확인 (또는 문제 수정)
2. `helm dependency update` 성공
3. `helm template` 렌더링 에러 0건 (인프라 + 서비스)
4. 인프라 Pod 전부 Running/Ready
5. 서비스 Pod 전부 Running/Ready (Gateway, Auth, Chat)
6. 인프라 상호 연결 정상
7. 서비스 → 인프라 연결 정상
8. `helm test` 통과
9. `local-setup.sh` / `local-teardown.sh` 정상 동작
10. 발견 이슈 전부 수정 완료
