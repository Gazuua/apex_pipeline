# 로드맵 재설계 — 설계 문서

**날짜**: 2026-03-08
**범위**: Phase 5~10 신규 설계 + 기존 문서 정합성 업데이트

---

## 1. 배경 및 문제점

v0.2.4 완료 후 기존 Phase 5~7 로드맵 검토 결과 7가지 문제점 식별:

1. **Phase 5 내부 의존성** — 로깅(KafkaSink)이 Kafka 어댑터에 의존, 4병렬 불가
2. **Phase 5 스코프 과다** — 외부 어댑터 4개를 하나의 Phase로 묶음
3. **Phase 6 스코프 과다** — Gateway + Auth가 한 에이전트 분량 아님
4. **Graceful Shutdown 배치 불일치** — 설계서 "v0.3.0 예정" vs 로드맵 Phase 6(v0.4.0)
5. **13개 항목 Phase 미할당** — WebSocket, Connection Pool, 안정성 패턴 등
6. **CI/CD 배치 과지연** — v1.0.0에 배치, 어댑터 개발 시점에 이미 필요
7. **Phase 간 난이도 불균형** — Phase 2(자료구조) vs Phase 5(외부 어댑터)

---

## 2. 설계 결정

| # | 결정 사항 | 선택 | 근거 |
|---|----------|------|------|
| 1 | 재설계 범위 | 로드맵 + 설계서 동시 업데이트 | 문서 간 모순 해소 |
| 2 | Phase 분할 기준 | 의존성 단위 | 실제 구현 순서를 의존성이 결정 |
| 3 | 누락 항목 처리 | 코어 → Phase, 서비스/성능 → 백로그 | 경직되지 않는 유연한 배치 |
| 4 | CI/CD 시점 | Phase 5 이전 선행 | 어댑터 개발의 안전망 |
| 5 | 넘버링 체계 | 완료분 동결 + 신규 순차 정수 | 기존 문서 참조 보존 |
| 6 | 버전 태깅 | 마일스톤 단위 | 버전 인플레이션 방지 |

---

## 3. 활성 로드맵 (Phase 5~10)

### 전체 의존성 그래프

```
Phase 5 (기반 정비)
    │
    ├──→ Phase 6 (Kafka 체인)  ─┐
    │                            ├──→ Phase 8 (Gateway 체인) → v0.4.0
    └──→ Phase 7 (데이터 체인) ─┘            │
              │                              │
              └──→ v0.3.0                    ▼
                                    Phase 9 (운영 인프라)
                                             │
                                             ▼
                                    Phase 10 (최종 통합) → v1.0.0
```

### Phase 5: 기반 정비

**목적**: 외부 어댑터 개발 전 CI 자동화 + 설정 인프라 + 미완성 코어 기능 완성

**구성**:
- CI/CD: GitHub Actions (빌드+테스트, Windows+Linux), docker-compose 통합 테스트 파이프라인
- TOML 설정: toml++ vcpkg 추가, ServerConfig TOML 로딩, 설정 파일 구조 설계
- Graceful Shutdown 타임아웃: TOML에서 drain_timeout 로딩, Server::stop() 적용, SIGHUP 로그 레벨 변경

**병렬 구조**: CI/CD ∥ TOML → Graceful Shutdown (2병렬 + 1순차)
**버전 태그**: 없음 (내부 기반 작업)
**완료 기준**: PR마다 CI 자동 실행, 에코 서버 TOML 설정 로딩, drain 타임아웃 동작, 기존 테스트 전수 통과

### Phase 6: Kafka 체인 (Phase 7과 병렬 가능)

**목적**: Kafka 연동 + 로깅 파이프라인 구축

**구성**:
- Kafka 어댑터: KafkaProducer 래퍼 (전역 공유, ADR-08), KafkaConsumer (파티션:코어 매핑), librdkafka fd → Asio 등록
- 로깅: spdlog 통합 (Console+File+KafkaSink), 구조화 JSON, trace_id 자동 주입

**내부 의존**: Kafka 어댑터 → 로깅 (KafkaSink가 KafkaProducer 사용)
**버전 태그**: Phase 7과 함께 v0.3.0

### Phase 7: 데이터 체인 (Phase 6과 병렬 가능)

**목적**: Redis/PG 연동 + Connection Pool

**구성**:
- Redis 어댑터: redis-plus-plus async (Asio 백엔드)
- PG 어댑터: libpq fd → Asio 등록, 비동기 쿼리 래퍼, Prepared Statement
- Connection Pool: Redis/PG 공통 풀 추상화, 코어별 인스턴스 (shared-nothing), health check + 재연결

**내부 의존**: Redis ∥ PG → Connection Pool (2병렬 + 1순차)
**버전 태그**: Phase 6과 함께 v0.3.0

### v0.3.0 완료 기준

- Kafka produce/consume이 Asio 이벤트 루프에서 동작
- spdlog → KafkaSink로 로그가 Kafka 토픽 도달
- Redis get/set, PG 쿼리가 비동기 동작
- Connection Pool 코어별 독립 인스턴스 동작
- docker-compose 환경에서 전체 통합 테스트 통과

### Phase 8: Gateway 체인

**목적**: WebSocket + Gateway + Auth — 전체 파이프라인 E2E 동작

**구성**:
- WebSocket 프로토콜: WebSocketProtocol (ProtocolBase CRTP 구현체), Beast 통합, ping/pong 하트비트 (ADR-06)
- Gateway: TLS 종단, JWT 로컬 검증 (ADR-07), 블룸필터 캐시, Kafka 라우팅, Rate Limiting (Redis 기반)
- Auth 서비스: JWT 발급/갱신 (jwt-cpp), Redis 블랙리스트, 블룸필터 갱신 Pub/Sub, PG 스키마

**내부 의존**: WebSocket → Gateway ∥ Auth
**버전 태그**: v0.4.0

### v0.4.0 완료 기준

- 클라이언트 WebSocket → Gateway → Kafka → Auth E2E 동작
- JWT 검증 + 블룸필터 + Redis 블랙리스트 인증 체인
- Rate Limiting 동작
- E2E 시나리오 테스트 (로그인 → 인증 요청 → 로그아웃 → 블랙리스트 확인)

### Phase 9: 운영 인프라

**목적**: 프로덕션 배포 준비 — 관측성 + 컨테이너화 + 오케스트레이션

**구성**:
- 메트릭: prometheus-cpp, 프레임워크 메트릭 노출, Grafana 대시보드
- Docker: 서비스별 Dockerfile (멀티스테이지), docker-compose full 프로파일
- K8s: Helm Chart, HPA, ConfigMap, Health Check probe
- CI/CD 고도화: Docker 빌드 + 이미지 푸시, 스테이징 배포, 서비스 스캐폴딩 스크립트

**병렬 구조**: 4작업 전부 독립 (완전 병렬)
**버전 태그**: 없음 (v1.0.0의 일부)

### Phase 10: 최종 통합

**목적**: 전체 검증 + v1.0.0 릴리스

**구성** (단일 세션):
- K8s E2E 테스트, 부하 테스트 (목표 TPS/Latency), 모니터링 대시보드 최종 구성, 문서 정리

**버전 태그**: v1.0.0

### v1.0.0 완료 기준

- K8s에서 다중 Pod 기동 + 롤링 업데이트
- Prometheus + Grafana 실시간 메트릭
- 부하 테스트 목표 달성
- GitHub Actions 이미지 빌드 → 배포 자동화
- 문서 정합성 최종 검증

---

## 4. 문서 정합성 업데이트 범위

### 설계 문서

| 문서 | 업데이트 내용 |
|------|-------------|
| `Apex_Pipeline.md` §4 | 최적화 항목 "v0.3.0"/"v0.4.0" → Phase 번호 통일 |
| `Apex_Pipeline.md` §5 | Graceful Shutdown "v0.3.0 예정" → "Phase 5" |
| `Apex_Pipeline.md` §8 | 기술 스택 상태 컬럼 Phase 번호로 갱신 |
| `Apex_Pipeline.md` §10 | 완료 이력 + 활성 로드맵 구조로 전면 재작성 |
| `Apex_Pipeline.md` TODO | Phase 할당 + 백로그 분리 |
| `design-decisions.md` | Phase 5~7 정의 → Phase 5~10 재작성 |
| `design-rationale.md` | ADR-05, ADR-14 Phase 참조 갱신 |

### 프로젝트 README

| 문서 | 업데이트 내용 |
|------|-------------|
| `apex_core/README.md` | 의존성 "v0.3.0+" → Phase 번호, 향후 라이브러리 Phase 매핑 |
| `apex_services/README.md` | 서비스별 구현 Phase 명시 (gateway/auth-svc → Phase 8) |
| `apex_infra/README.md` | K8s "향후 추가" → Phase 9 명시 |
| `apex_tools/README.md` | new-service.sh "예정" → Phase 9 명시 |
| `apex_shared/README.md` | 변경 불필요 |

---

## 5. 넘버링/버전 규칙

- **완료 Phase (1~4.7)**: 레거시 넘버링 동결. 상세는 `docs/apex_core/progress/` 참조
- **활성 Phase (5+)**: 순차 정수
- **버전 태깅**: 마일스톤 단위 (v0.3.0 = Phase 6+7, v0.4.0 = Phase 8, v1.0.0 = Phase 9+10)
- **Phase 간 병렬**: Phase 6 ∥ Phase 7 명시적 표기
