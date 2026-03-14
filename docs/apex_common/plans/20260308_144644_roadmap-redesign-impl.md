# 로드맵 재설계 — 문서 정합성 업데이트 구현 계획

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 로드맵 재설계 설계 문서(`20260308_144644_roadmap-redesign.md`) 기반으로 기존 설계서/README 7개 파일의 Phase 참조와 로드맵 구조를 일괄 갱신한다.

**Architecture:** 각 Task는 독립 파일을 수정하므로 Task 1~5는 병렬 실행 가능. Task 6(검증)만 전체 완료 후 순차 실행. 모든 작업은 `feature/roadmap-redesign` 워크트리(`D:/.workspace/.worktrees/roadmap-redesign/`)에서 수행.

**Tech Stack:** Markdown 문서 편집 전용. 코드 변경 없음.

**참조 문서:** `docs/apex_common/plans/20260308_144644_roadmap-redesign.md` (설계 문서)

---

## Task 1: Apex_Pipeline.md 전면 갱신 (§4, §5, §8, §10, TODO)

**가장 큰 작업. 5개 섹션을 순차 수정.**

**Files:**
- Modify: `docs/Apex_Pipeline.md`

### Step 1: §10 로드맵 구조 전면 재작성

현재 §10 `개발 진행 상황`의 `apex_core 로드맵` 테이블과 `전체 TODO`를 아래 구조로 **전면 교체**:

```markdown
### apex_core 로드맵

#### 완료 이력 (v0.1.0 ~ v0.2.4)

> Phase 1~4.7은 레거시 넘버링. 상세는 docs/apex_core/progress/ 참조.

| Phase | 버전 | 요약 | 테스트 |
|-------|------|------|--------|
| 1~2 | - | 프로젝트 셋업 + 기반 컴포넌트 (MPSC, SlabPool, RingBuffer, TimingWheel) | 29개 |
| 3~3.5 | v0.1.0 | 코어 프레임워크 통합 (CoreEngine, ServiceBase, WireHeader, FrameCodec) | 누적 69개 |
| 4~4.5 | v0.2.0 | 프로토콜, 세션, Server 통합, E2E 테스트 | 누적 98개 |
| 4.5-r | v0.2.1 | 코루틴 전환 + 코드 리뷰 2회 수정 | 누적 106개 |
| 4.6 | v0.2.2 | ProtocolBase CRTP + 에러 전파 파이프라인 | +18 스위트 |
| 4.6-r | v0.2.3 | v0.2.2 코드 리뷰 수정 | +5개 |
| 4.7 | v0.2.4 | Server-CoreEngine 통합, cross_core_call, Graceful Shutdown | +24 케이스 + 리뷰 추가 15건 |

#### 활성 로드맵 (Phase 5+)

> Phase 5부터 순차 정수. 각 Phase = 하나의 의존성 묶음.
> Phase 간 병렬 관계는 ∥ 표기.

| Phase | 버전 | 내용 | 의존 |
|-------|------|------|------|
| 5 | - | 기반 정비: CI/CD + TOML 설정 + Graceful Shutdown 타임아웃 | - |
| 6 | v0.3.0 | Kafka 체인: Kafka 어댑터 → 로깅 (spdlog + KafkaSink + trace_id) | Phase 5 |
| 7 | v0.3.0 | 데이터 체인: Redis 어댑터 ∥ PG 어댑터 → Connection Pool | Phase 5 |
| 8 | v0.4.0 | Gateway 체인: WebSocket + Gateway + Auth 서비스 | Phase 6 + 7 |
| 9 | - | 운영 인프라: 메트릭 + Docker + K8s + CI/CD 고도화 | Phase 8 |
| 10 | v1.0.0 | 최종 통합: E2E 검증 + 부하 테스트 + 문서 정리 | Phase 9 |

> Phase 6 ∥ Phase 7 — 서로 독립, 동시 진행 가능. 둘 다 완료 시 v0.3.0 태그.
```

### Step 2: §4 속도 최적화 Phase 참조 갱신

`속도 최적화 설계` 테이블에서 상태 컬럼 수정:

| 항목 | 현재 | 변경 |
|------|------|------|
| Connection Pool | `v0.3.0` | `Phase 7` |
| Kafka Batch Produce | `v0.3.0` | `Phase 6` |
| CPU Affinity + SO_REUSEPORT | `미착수` | `백로그 (Phase 10 부하 테스트 후)` |
| L1 로컬 캐시 | `v0.4.0` | `백로그 (Phase 10 부하 테스트 후)` |
| 코루틴 프레임 풀 할당 | `미착수` | `백로그 (ADR-21, 벤치마크 후)` |
| 배치 I/O (writev) | `미착수` | `백로그 (Phase 10 부하 테스트 후)` |
| epoll/io_uring | `미착수` | `백로그 (ADR-15, Linux 배포 후)` |

### Step 3: §5 안정성 설계 Graceful Shutdown 갱신

Graceful Shutdown 행의 설명에서:
- 현재: `"drain 타임아웃: ADR-05에서 기본값 25초(K8s 30초 대비 5초 여유)로 설계 확정, 구현은 v0.3.0 예정"`
- 변경: `"drain 타임아웃: ADR-05에서 기본값 25초(K8s 30초 대비 5초 여유)로 설계 확정, Phase 5에서 구현"`

### Step 4: §8 기술 스택 상태 갱신

| 기술 | 현재 상태 | 변경 |
|------|----------|------|
| librdkafka | `v0.3.0 예정` | `Phase 6` |
| redis-plus-plus | `v0.3.0 예정` | `Phase 7` |
| libpq | `v0.3.0 예정` | `Phase 7` |
| prometheus-cpp | `v0.4.0 예정` | `Phase 9` |
| jwt-cpp | `v0.4.0 예정` | `Phase 8` |
| Docker | `v1.0.0 예정` | `Phase 9` |
| Kubernetes | `v1.0.0 예정` | `Phase 9~10` |

### Step 5: TODO 섹션 재구성

현재 TODO 리스트를 **Phase 할당 + 백로그** 구조로 재작성:

```markdown
### 전체 TODO

#### 완료
- [x] apex_core 기반 컴포넌트 ~ v0.2.4 (상세는 완료 이력 참조)

#### Phase 5: 기반 정비
- [ ] CI/CD (GitHub Actions — 빌드+테스트, docker-compose 통합 테스트)
- [ ] TOML 설정 로딩 (toml++)
- [ ] Graceful Shutdown drain 타임아웃 (TOML 설정 가능)

#### Phase 6: Kafka 체인
- [ ] Kafka 어댑터 (librdkafka fd → Asio, Producer 공유 + Consumer 분리)
- [ ] 로깅 (spdlog + KafkaSink + trace_id)

#### Phase 7: 데이터 체인
- [ ] Redis 어댑터 (redis-plus-plus async)
- [ ] PostgreSQL 어댑터 (libpq → Asio)
- [ ] Connection Pool (코어별 독립, health check)

#### Phase 8: Gateway 체인
- [ ] WebSocket 프로토콜 (Boost.Beast, ProtocolBase 구현체)
- [ ] Gateway (TLS, JWT, 블룸필터, Kafka 라우팅, Rate Limiting)
- [ ] Auth 서비스 (JWT 발급/검증/블랙리스트)
- [ ] FlatBuffers 공유 메시지 스키마 정의

#### Phase 9: 운영 인프라
- [ ] Prometheus 메트릭 노출
- [ ] Docker 멀티스테이지 빌드
- [ ] K8s manifests + Helm
- [ ] CI/CD 고도화 (이미지 빌드 + 배포)
- [ ] 서비스 스캐폴딩 스크립트

#### Phase 10: 최종 통합
- [ ] K8s E2E 테스트
- [ ] 부하 테스트 시나리오 (목표 TPS/Latency)
- [ ] 모니터링 대시보드 구성

#### 백로그 (해당 Phase에서 흡수)
- [ ] Circuit Breaker (→ Phase 8)
- [ ] Dead Letter Queue (→ Phase 6)
- [ ] Idempotency Key (→ Phase 8)
- [ ] Kafka 토픽/파티션 설계 (→ Phase 6 브레인스토밍)
- [ ] Redis 키 네이밍 (→ Phase 7 브레인스토밍)
- [ ] PostgreSQL 스키마 설계 (→ Phase 8 브레인스토밍)

#### 백로그 — 성능 최적화 (벤치마크 후)
- [ ] CPU Affinity + SO_REUSEPORT
- [ ] 배치 I/O (writev)
- [ ] 코루틴 프레임 풀 할당 (ADR-21)
- [ ] L1 로컬 캐시
- [ ] epoll → io_uring (ADR-15)
```

### Step 6: 커밋

```bash
git add docs/Apex_Pipeline.md
git commit -m "docs: Apex_Pipeline.md 로드맵 재설계 반영 — §4/§5/§8/§10/TODO Phase 참조 전면 갱신"
```

---

## Task 2: design-decisions.md Phase 정의 재작성

**Files:**
- Modify: `docs/apex_core/design-decisions.md`

### Step 1: 구현 로드맵 섹션 교체

`## 구현 로드맵 (에이전트 팀 병렬 기반)` 섹션에서 Phase 5~7.5 정의를 Phase 5~10으로 교체.

기존 Phase 1~4.5는 그대로 유지. Phase 5 이후를 아래로 교체:

```markdown
### Phase 5: 기반 정비 (CI/CD + 설정 + Graceful Shutdown)
- CI/CD: GitHub Actions (빌드+테스트 자동화, docker-compose 통합 테스트)
- TOML 설정: toml++ 통합, ServerConfig TOML 로딩, 설정 파일 구조
- Graceful Shutdown: TOML에서 drain_timeout 로딩, Server::stop() 적용, SIGHUP 로그 레벨

### Phase 6: Kafka 체인 (Phase 7과 병렬 가능)
- Kafka 어댑터: KafkaProducer 래퍼 (전역 공유, ADR-08), KafkaConsumer (파티션:코어 매핑), librdkafka fd → Asio
- 로깅: spdlog 통합 (Console+File+KafkaSink), 구조화 JSON, trace_id 자동 주입
- 내부 의존: Kafka 어댑터 → 로깅

### Phase 7: 데이터 체인 (Phase 6과 병렬 가능)
- Redis 어댑터: redis-plus-plus async (Asio 백엔드)
- PG 어댑터: libpq fd → Asio 등록, 비동기 쿼리 래퍼
- Connection Pool: 공통 풀 추상화, 코어별 인스턴스 (shared-nothing), health check
- 내부 의존: Redis ∥ PG → Connection Pool

### Phase 7.5: 어댑터 통합 (단일 세션)
- Phase 6 + 7 어댑터 통합 테스트 → **v0.3.0**

### Phase 8: Gateway 체인
- WebSocket 프로토콜: WebSocketProtocol (ProtocolBase CRTP), Beast 통합, ping/pong (ADR-06)
- Gateway: TLS 종단, JWT 검증 (ADR-07), 블룸필터, Kafka 라우팅, Rate Limiting
- Auth 서비스: JWT 발급/갱신, Redis 블랙리스트, 블룸필터 Pub/Sub, PG 스키마
- 내부 의존: WebSocket → Gateway ∥ Auth

### Phase 8.5: 파이프라인 통합 (단일 세션)
- E2E 통합 테스트 → **v0.4.0**

### Phase 9: 운영 인프라 (4작업 완전 병렬)
- 메트릭: prometheus-cpp, Grafana 대시보드
- Docker: 서비스별 Dockerfile (멀티스테이지)
- K8s: Helm Chart, HPA, ConfigMap, Health Check
- CI/CD 고도화: Docker 빌드 + 배포, 스캐폴딩 스크립트

### Phase 10: 최종 통합 (단일 세션)
- K8s E2E, 부하 테스트, 문서 정리 → **v1.0.0**
```

### Step 2: Graceful Shutdown 참조 갱신

`### Graceful Shutdown` 섹션 (L184~186 부근)에서:
- 현재: `"(미구현, v0.3.0에서 구현 예정)"`
- 변경: `"(Phase 5에서 구현)"`

### Step 3: 외부 의존성 참조 갱신

`### 개발 편의` 섹션 마지막 줄 (L207 부근)에서:
- 현재: `"나머지는 해당 Phase에서 추가 예정"`
- 변경: `"나머지는 해당 Phase에서 추가 (Kafka/spdlog → Phase 6, Redis/libpq → Phase 7, jwt-cpp/prometheus-cpp → Phase 8~9)"`

### Step 4: 커밋

```bash
git add docs/apex_core/design-decisions.md
git commit -m "docs: design-decisions.md Phase 5~10 재작성 + 참조 갱신"
```

---

## Task 3: design-rationale.md ADR 참조 갱신

**Files:**
- Modify: `docs/apex_core/design-rationale.md`

### Step 1: ADR-05 Phase 참조 갱신

ADR-05 `Graceful Shutdown 상세` 섹션 (L154 부근)에서:
- 현재: `"**C. 설정 가능, 기본값 25초** — TOML에서 오버라이드 가능 (미구현, v0.3.0에서 구현 예정)"`
- 변경: `"**C. 설정 가능, 기본값 25초** — TOML에서 오버라이드 가능 (Phase 5에서 구현)"`

### Step 2: ADR-14 Phase 참조 갱신

ADR-14 `구현 로드맵` 섹션 (L421~434 부근)의 근거 목록은 변경 불필요 — 원칙(병렬 구현 → 통합 → 태그)은 동일. 단, Phase 번호가 언급되어 있다면 갱신.

ADR-14를 확인한 결과, 구체적 Phase 번호는 언급하지 않고 원칙만 기술 → **변경 불필요**.

### Step 3: 커밋

```bash
git add docs/apex_core/design-rationale.md
git commit -m "docs: design-rationale.md ADR-05 Phase 참조 갱신"
```

---

## Task 4: README 파일 4개 갱신

**Files:**
- Modify: `apex_core/README.md`
- Modify: `apex_services/README.md`
- Modify: `apex_infra/README.md`
- Modify: `apex_tools/README.md`

### Step 1: apex_core/README.md

의존성 섹션에서:
- 현재: `"> 향후 v0.3.0+에서 추가 예정: librdkafka, redis-plus-plus, libpq, prometheus-cpp, jwt-cpp"`
- 변경: `"> 향후 추가 예정: librdkafka, spdlog의 KafkaSink (Phase 6), redis-plus-plus, libpq (Phase 7), prometheus-cpp (Phase 9), jwt-cpp (Phase 8)"`

### Step 2: apex_services/README.md

서비스 테이블에 Phase 컬럼 추가:

```markdown
| 서비스 | 역할 | 구현 Phase |
|--------|------|-----------|
| gateway | WebSocket/HTTP 게이트웨이 | Phase 8 |
| auth-svc | 인증/인가 | Phase 8 |
| chat-svc | 채팅 로직 | 미정 |
| log-svc | 로그 수집/저장 | 미정 |
```

### Step 3: apex_infra/README.md

K8s 섹션에서:
- 현재: `"- \`k8s/\` — Helm charts (서비스별, 향후 추가)"`
- 변경: `"- \`k8s/\` — Helm charts (서비스별, Phase 9에서 추가)"`

### Step 4: apex_tools/README.md

기타 섹션에서:
- 현재: `"- \`new-service.sh\` — 서비스 스캐폴딩 스크립트 (예정)"`
- 변경: `"- \`new-service.sh\` — 서비스 스캐폴딩 스크립트 (Phase 9에서 추가)"`

### Step 5: 커밋

```bash
git add apex_core/README.md apex_services/README.md apex_infra/README.md apex_tools/README.md
git commit -m "docs: README 4개 Phase 참조 정합성 갱신"
```

---

## Task 5: CLAUDE.md Phase 참조 갱신

**Files:**
- Modify: `CLAUDE.md` (프로젝트 루트)

### Step 1: 의존성 참조 갱신

빌드 환경 > 의존성 섹션에서:
- 현재: `"boost-beast (v0.3.0 Gateway WebSocket용)"`
- 변경: `"boost-beast (Phase 8 Gateway WebSocket용)"`

### Step 2: 커밋

```bash
git add CLAUDE.md
git commit -m "docs: CLAUDE.md boost-beast Phase 참조 갱신"
```

---

## Task 6: 교차 검증 (전체 완료 후 실행)

**의존**: Task 1~5 전부 완료 후

### Step 1: Phase 번호 누락 검색

모든 설계 문서에서 `v0.3.0 예정`, `v0.4.0 예정`, `v1.0.0 예정`, `미착수` 등 구버전 표현이 남아있는지 검색:

```bash
grep -rn "v0\.3\.0 예정\|v0\.4\.0 예정\|v1\.0\.0 예정" docs/ apex_*/README.md CLAUDE.md
```

예상 결과: 매칭 없음. 매칭이 있으면 해당 라인을 Phase 번호로 수정.

### Step 2: Phase 참조 일관성 확인

활성 로드맵의 Phase 5~10이 모든 문서에서 동일하게 참조되는지 확인:

```bash
grep -rn "Phase [5-9]\|Phase 10" docs/ apex_*/README.md CLAUDE.md
```

각 Phase 번호가 설계 문서(`20260308_144644_roadmap-redesign.md`)의 정의와 일치하는지 대조.

### Step 3: 문제 발견 시 수정 + 커밋

```bash
git add -A
git commit -m "docs: Phase 참조 교차 검증 — 누락/불일치 수정"
```

(수정 사항 없으면 커밋 생략)

---

## 병렬 실행 가이드

```
Task 1 (Apex_Pipeline.md)  ─┐
Task 2 (design-decisions.md)│
Task 3 (design-rationale.md)├── 모두 독립 파일 → 5병렬 가능
Task 4 (README x4)         │
Task 5 (CLAUDE.md)         ─┘
                             │
                             ▼
                    Task 6 (교차 검증) ← 전체 완료 후 순차
```
