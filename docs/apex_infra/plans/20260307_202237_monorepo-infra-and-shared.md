# 모노레포 인프라 구축 계획 — 인프라 관점

**작성일**: 2026-03-07
**범위**: apex_infra 인프라 설정 (Docker, Kafka, Redis, PostgreSQL, 모니터링)
**관련 문서**: `docs/apex_shared/plans/20260307_202237_monorepo-infra-and-shared.md` (공유 라이브러리 관점)

---

## 1. ~~루트 빌드 메타데이터 버전 동기화~~ ✅ 완료

커밋: `8c64692` — CMakeLists.txt, vcpkg.json 버전 0.2.2 → 0.2.3

---

## 2. ~~설계 문서 현행화 (Apex_Pipeline.md)~~ ✅ 완료

커밋: `8c64692` — docker-compose 프로파일 구조 정정 (minimal/observability/full)

---

## 3. apex_infra — docker-compose.yml 작성 (핵심 산출물)

**목적**: 로컬 개발 환경에서 외부 의존성(Kafka, Redis, PostgreSQL)을 한 번에 기동

**프로파일 구조** (설계 문서 §9 기준):

| 프로파일 | 서비스 |
|----------|--------|
| `minimal` | Kafka (KRaft), Redis, PostgreSQL |
| `observability` | minimal + Prometheus + Grafana |
| `full` | observability + 서비스 컨테이너 전체 (향후) |

**주요 인프라 결정 사항**:
- **Kafka**: KRaft 모드 (ZooKeeper 제거), 단일 브로커로 시작. K8s 환경에서 3 브로커로 확장 예정
- **Redis**: 단일 노드 (로컬 개발용). 프로덕션에서는 클러스터 모드로 전환
- **PostgreSQL**: 16+, schema-per-service 초기화 스크립트 포함 (auth_schema, chat_schema, match_schema)
- **네트워크**: `apex-net` 브릿지 네트워크로 컨테이너 간 서비스 디스커버리
- **볼륨**: 명명된 볼륨으로 데이터 영속화 (재시작 시 데이터 유지)
- **모니터링**: Prometheus 스크래핑 설정 + Grafana 데이터소스 자동 프로비저닝

**산출물**:
- `apex_infra/docker-compose.yml` — 프로파일 기반 서비스 정의
- `apex_infra/postgres/init.sql` — 서비스별 스키마 초기화 (CREATE SCHEMA)
- `apex_infra/prometheus/prometheus.yml` — Prometheus 타겟 설정
- `apex_infra/grafana/provisioning/` — Grafana 데이터소스 자동 등록
- `apex_infra/README.md` — 사용법 (docker compose up, 프로파일 선택)

**포트 매핑**:

| 서비스 | 포트 |
|--------|------|
| Kafka | 9092 |
| Redis | 6379 |
| PostgreSQL | 5432 |
| Prometheus | 9090 |
| Grafana | 3000 |

---

## 4. 인프라와 공유 스키마의 관계

apex_shared에서 정의하는 FlatBuffers 메시지 스키마는 Kafka 토픽을 통해 전달되는 메시지의 직렬화 포맷을 결정한다. 인프라 관점에서의 고려사항:

- Kafka 토픽 파티션 설계는 메시지 타입(auth, chat, log)에 맞춰 결정
- PostgreSQL 스키마 초기화는 서비스 도메인(auth_schema, chat_schema)과 1:1 대응
- 공유 스키마의 상세 내용은 `docs/apex_shared/plans/` 참조

---

## 충돌 방지 규칙

apex_core 에이전트와의 파일 충돌을 방지하기 위해:

- **수정 금지**: `apex_core/` 하위 모든 파일
- **수정 가능**: `apex_infra/`, 루트 `docker-compose.yml` 관련 파일
- 루트 `CMakeLists.txt`의 `add_subdirectory(apex_core)` 라인은 변경하지 않음
