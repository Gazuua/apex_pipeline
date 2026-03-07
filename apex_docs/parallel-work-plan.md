# Apex Pipeline - 병행 작업 계획

**작성일:** 2026-03-07
**기준:** BoostAsioCore v0.2.1 (Phase 4.5 완료, 2차 코드 리뷰 완료)

---

## 현재 상황

- **BoostAsioCore**: v0.2.1 완료 (Phase 4.5), 다른 에이전트가 Phase 5 진행 중
- **Phase 5 목표**: 외부 어댑터 (Kafka, Redis, PostgreSQL, 로깅) → v0.3.0
- **workspace 상태**: `BoostAsioCore/` 폴더만 존재, 나머지 프로젝트 구조 미생성

---

## 병행 가능 작업 목록

BoostAsioCore 개발과 의존성이 없어 별도 세션에서 즉시 착수 가능한 작업들.

---

### P1. 최우선 (Phase 5 진입 전 완료 시 시너지 큼)

#### P1-1. docker-compose.yml (minimal 프로파일)

- **경로:** `infra/docker-compose.yml`
- **내용:** Kafka(KRaft 모드), Redis 7+, PostgreSQL 16+를 로컬에서 한 번에 기동
- **프로파일 구성:**
  - `minimal`: Kafka, Redis, PostgreSQL만 기동 (개발용)
  - `observability`: + Prometheus, Grafana (모니터링 확인용)
  - `full`: + 모든 서비스 컨테이너 (통합 테스트용)
- **의존성:** 없음 (순수 인프라 구성)
- **가치:** Phase 5에서 Kafka/Redis/PG 어댑터 개발 시 즉시 연동 테스트 가능. 이게 없으면 어댑터 개발이 mock 기반으로만 진행되어 통합 검증이 늦어짐.

#### P1-2. FlatBuffers 공유 메시지 스키마 정의

- **경로:** `shared/schemas/`
- **내용:** 서비스 간 통신에 사용할 FlatBuffers 메시지 정의
- **주요 스키마:**
  - `common.fbs`: 공통 타입 (ErrorResponse, Timestamp, UUID 등)
  - `auth.fbs`: 인증 관련 (LoginRequest/Response, TokenRefresh, LogoutRequest 등)
  - `chat.fbs`: 채팅 관련 (SendMessage, JoinRoom, LeaveRoom, BroadcastMessage 등)
  - `gateway.fbs`: Gateway 내부 라우팅용 (RouteEnvelope, Heartbeat 등)
- **설계 기준:**
  - 와이어 프로토콜 `msg_id(u16)`와 1:1 매핑
  - `route<T>` 디스패치에 직접 사용되는 타입
  - zero-copy 읽기를 위한 FlatBuffers 모범 사례 준수
- **의존성:** 없음 (BoostAsioCore의 FlatBuffers 인프라는 이미 Phase 4에서 완성)
- **가치:** 서비스 간 "계약"이 확정되어야 Gateway/Service 핸들러 시그니처가 결정됨

#### P1-3. 데이터 계층 설계 (Kafka + Redis + PostgreSQL)

- **경로:** `docs/data-layer-design.md` 또는 각각 분리
- **내용:**

**Kafka 토픽/파티션 설계:**
  - 토픽 네이밍 규칙 (예: `apex.gateway.inbound`, `apex.auth.request`, `apex.chat.broadcast`)
  - 파티션 전략 (session_id 기반 파티셔닝 → 순서 보장)
  - Consumer Group 설계 (서비스별 독립 그룹)
  - DLQ 토픽 규칙 (예: `apex.auth.request.dlq`)
  - Retention / Compaction 정책

**Redis 키 네이밍 규칙:**
  - `Apex_Pipeline.md` 6절 초안 기반 상세화
  - `auth:session:{session_id}`, `auth:token:{user_id}`, `auth:bloom:{partition}`
  - `chat:room:{room_id}`, `chat:msg:{room_id}:{seq}`
  - TTL 정책, Pub/Sub 채널 네이밍
  - L1 캐시 무효화 프로토콜

**PostgreSQL 스키마 설계:**
  - `auth_schema`: users, tokens, sessions DDL
  - `chat_schema`: rooms, messages, members DDL
  - 인덱스 전략, 파티셔닝 (messages 테이블 등)
  - Repository 추상화 인터페이스 (C++ 헤더 수준)

- **의존성:** 없음 (순수 설계)
- **가치:** Phase 5 외부 어댑터가 접속할 "대상"을 미리 정의

---

### P2. 중요 (언제든 가능, 빠를수록 좋음)

#### P2-1. GitHub Actions CI/CD

- **경로:** `.github/workflows/`
- **내용:**
  - BoostAsioCore 빌드 + 테스트 자동화 (이미 `build.sh`, `build.bat`, CMakePresets.json 존재)
  - vcpkg 캐싱 (Boost, GTest 빌드 시간 절약)
  - clang-tidy / ASAN / TSAN 옵션
  - PR 트리거 + main 푸시 트리거
- **의존성:** 없음
- **가치:** 코드 품질 자동 검증, 회귀 방지

#### P2-2. Gateway 상세 설계 문서

- **경로:** `docs/gateway-design.md`
- **내용:**
  - TLS 종단 처리 흐름 (Boost.Beast SSL)
  - JWT 로컬 검증 흐름 (jwt-cpp)
  - 블룸필터 기반 토큰 블랙리스트 체크
  - msg_id → Kafka 토픽 라우팅 테이블
  - Rate Limiting (Token Bucket, Redis 기반 분산)
  - Connection Draining (롤링 업데이트 시)
- **의존성:** FlatBuffers 스키마(P1-2)가 확정되면 더 구체화 가능
- **가치:** Phase 6 Gateway 구현 시 설계 문서 기반으로 빠르게 착수

#### P2-3. Auth 서비스 상세 설계 문서

- **경로:** `docs/auth-service-design.md`
- **내용:**
  - JWT 발급/갱신/폐기 흐름
  - 세션 관리 (Redis + PostgreSQL 이중 저장)
  - 블룸필터 갱신 프로토콜 (Redis Pub/Sub → 전 Gateway 동기화)
  - 비밀번호 해싱 전략 (Argon2 / bcrypt)
  - OAuth2 확장 가능성
- **의존성:** 데이터 계층 설계(P1-3)가 확정되면 더 구체화 가능
- **가치:** Phase 6 Auth 서비스 구현 시 설계 문서 기반으로 빠르게 착수

---

### P3. 나중에 해도 됨 (Phase 6~7 범위)

#### P3-1. K8s Helm Charts

- **경로:** `infra/k8s/`
- **내용:** Gateway, Auth, Chat 서비스별 Deployment + Service + HPA
- **의존성:** Dockerfile이 먼저 필요 (Phase 7 범위)
- **가치:** 프로덕션 배포 준비

#### P3-2. 모니터링 대시보드 설계

- **경로:** `infra/grafana/` 또는 `docs/monitoring-design.md`
- **내용:** Prometheus 메트릭 정의, Grafana 대시보드 JSON
- **의존성:** prometheus-cpp 통합 (Phase 5~6)
- **가치:** 운영 가시성

#### P3-3. 부하 테스트 시나리오

- **경로:** `docs/load-test-plan.md`
- **내용:** 목표 TPS/Latency 정의, 테스트 도구 선정, 시나리오 설계
- **의존성:** 최소 v0.3.0 이상에서 의미 있음
- **가치:** 성능 검증 기준 확립

---

## 작업 간 의존 관계

```
P1-1 (docker-compose) ──────────────────────────────────┐
                                                        ├→ Phase 5 (외부 어댑터)
P1-2 (FlatBuffers 스키마) ──┬───────────────────────────┘
                            │
P1-3 (데이터 계층 설계) ────┤
                            │
                            ├→ P2-2 (Gateway 설계) ──→ Phase 6 (Gateway 구현)
                            │
                            └→ P2-3 (Auth 설계) ────→ Phase 6 (Auth 구현)

P2-1 (CI/CD) ──→ 독립 (언제든 적용 가능)

P3-* ──→ Phase 6~7 범위
```

---

## 병렬 세션 구성 제안

| 세션 | 작업 | 비고 |
|------|------|------|
| 세션 A | BoostAsioCore Phase 5 (진행 중) | 다른 에이전트가 담당 |
| 세션 B | P1-1 + P1-3 (인프라 + 데이터 설계) | docker-compose → Kafka/Redis/PG 설계 순서 |
| 세션 C | P1-2 (FlatBuffers 스키마) | 메시지 계약 확정 |
| 세션 D | P2-1 (CI/CD) | 독립 작업 |

세션 B, C, D는 서로 독립적이므로 동시 진행 가능.
P2-2, P2-3은 P1-2, P1-3 완료 후 착수하는 것이 효율적.
