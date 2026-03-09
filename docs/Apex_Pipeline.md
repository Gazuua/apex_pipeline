# Apex Pipeline - 설계 문서

**범용 고성능 실시간 서버 파이프라인**
설계 원칙: 최고 속도 + 최고 안정성 + 자유로운 스케일아웃

---

## 1. 아키텍처

```
┌────────────────────────────────────────────────────┐
│ K8s Cluster                                        │
│                                                    │
Clients  │  ┌─────────────┐     ┌─────────────┐     │
(WS/TCP)─────┤──►│ Gateway x N │     │ Gateway x N │     │
│  │ (C++/Asio) │     │ (C++/Asio) │     │
│  └──┬──────────┘     └──┬──────────┘     │
│     │                   │                │
│     │       ┌───────────┘                │
│     ▼       ▼                            │
│  ┌────────────────────────────┐          │
│  │     Kafka Cluster          │          │
│  │   (topic per service)      │          │
│  └──┬──────────┬──────────┬───┘          │
│     │          │          │              │
│     ▼          ▼          ▼              │
│  ┌────────┐ ┌────────┐ ┌────────┐       │
│  │Svc A   │ │Svc B   │ │Svc C   │ ←스케일│
│  │(C++) xN│ │(C++) xN│ │(C++) xN│  아웃 │
│  └─┬────┬─┘ └─┬────┬─┘ └─┬────┬─┘       │
│    │    │      │    │      │    │         │
│    ▼    ▼      ▼    ▼      ▼    ▼         │
│  ┌──────────────────────────────────────┐ │
│  │ Redis Cluster    │ PostgreSQL        │ │
│  │ (핫데이터/캐시)  │ (영속 데이터)     │ │
│  │ ns별 논리 분리   │ schema별 논리분리 │ │
│  └──────────────────────────────────────┘ │
│                                            │
│  ┌──────────────────────────────────────┐ │
│  │   Prometheus + Grafana (모니터링)    │ │
│  └──────────────────────────────────────┘ │
└────────────────────────────────────────────────────┘
```

---

## 2. 통신 경로 (Kafka 중앙 버스)

```
[전 구간 통합]
Client → Gateway ──(Kafka)──→ Service ──(Kafka)──→ Gateway → Client

- Gateway가 자체 프레임워크(apex_core)로 클라이언트와 직접 통신
- 서비스 간 통신은 전부 Kafka 중앙 버스를 경유
- 즉시 응답이 필요한 경우: Gateway가 로컬에서 처리 (인증 검증 등)
```

> **설계 변경 근거**: gRPC 제거. 자체 프레임워크(apex_core)가 모든 네트워킹을 담당하며, 서비스 간 통신은 Kafka 중앙 버스로 통일. 동기/비동기 분리의 복잡도를 제거하고, Kafka의 내구성 + fan-out 특성을 전 구간에 활용.

---

## 3. 구간별 프로토콜

| 구간 | 프로토콜 | 라이브러리 |
|------|----------|-----------|
| Client ↔ Gateway | WebSocket / TCP Binary | Boost.Asio + Beast (apex_core) |
| Gateway ↔ Kafka | Kafka 네이티브 | librdkafka |
| Kafka ↔ Service | Kafka 네이티브 | librdkafka |
| Service ↔ Redis | Redis 프로토콜 | redis-plus-plus (hiredis) |
| Service ↔ DBMS | PostgreSQL 프로토콜 | libpq (Asio 어댑터 직접 구현) |
| **전 구간 직렬화** | **FlatBuffers** | flatbuffers (zero-copy) |

---

## 4. 속도 최적화 설계

| 기법 | 적용 위치 | 효과 | 상태 |
|------|----------|------|------|
| **io_context-per-core (shared-nothing)** | 전 서비스 | 코어별 독립 이벤트 루프, 락 제거 | ✅ Server 통합 완료 (ContextProvider 기반 per-core IOCP 직접 바인딩 완료) |
| **Lock-free MPSC Queue** | 코어 간 통신 | 코어당 수신 큐 1개, O(1) enqueue | ✅ 구현 |
| **Slab Memory Pool** | 코어별 독립 | 핫패스 malloc 제거, O(1) 할당 | ✅ 구현 |
| **Zero-copy Ring Buffer** | 수신 버퍼 | memmove 제거, FlatBuffers 직접 접근 | ✅ 구현 |
| **FlatBuffers 직렬화** | 전 구간 | 역직렬화 비용 제로 (zero-copy 읽기) | ✅ 구현 |
| **Connection Pool** | Service → Redis/PostgreSQL | 커넥션 생성 비용 제거 | Phase 7 |
| **Kafka Batch Produce** | Gateway → Kafka | 네트워크 라운드트립 최소화 | Phase 6 |
| **CPU Affinity + SO_REUSEPORT** | Gateway / Logic Service | 커널 레벨 코어 분배, 캐시 히트율 극대화 | 백로그 (Phase 10 부하 테스트 후) |
| **L1 로컬 캐시** | 전 서비스 | 프로세스 내 해시맵(TTL) → Redis 호출도 제거 | 백로그 (Phase 10 부하 테스트 후) |
| **코루틴 프레임 풀 할당** | 핫패스 | promise_type operator new → 슬랩 풀 할당 | 백로그 (ADR-21, 벤치마크 후) |
| **배치 I/O (writev)** | 전 서비스 | 시스콜 횟수 최소화 | 백로그 (Phase 10 부하 테스트 후) |
| **epoll 기본 / io_uring 선택** | I/O 백엔드 | CMake 옵션으로 io_uring 활성화 가능 | 백로그 (ADR-15, Linux 배포 후) |

---

## 5. 안정성 설계

| 패턴 | 적용 위치 | 동작 |
|------|----------|------|
| **Circuit Breaker** | 외부 서비스 호출 | 연속 실패 시 빠른 실패 반환, 연쇄 장애 차단 |
| **Dead Letter Queue** | Kafka Consumer | 처리 실패 메시지를 DLQ 토픽으로 격리, 유실 방지 |
| **Retry + Exponential Backoff** | 모든 외부 호출 | 일시적 실패 자동 재시도, 부하 폭주 방지 |
| **Graceful Shutdown** | 전 서비스 | SIGTERM → acceptor 중지 → 코어별 세션 close(코어 스레드에 비동기 post) → 세션 drain 폴링(active_sessions==0 대기, 1ms 주기) → CoreEngine stop → CoreEngine join → drain_remaining(잔여 MPSC 메시지 소비) → 서비스 정지 → shutdown_logging() → 종료. drain 타임아웃: ADR-05에서 기본값 25초(K8s 30초 대비 5초 여유)로 설계 확정, Phase 5에서 구현. shutdown 후 로깅 시도는 spdlog::get() null 체크로 방어 |
| **Health Check** | K8s Liveness/Readiness | 비정상 Pod 자동 재시작 |
| **Rate Limiting** | Gateway | Token Bucket, Redis 기반 분산 레이트리밋 |
| **Idempotency Key** | 전 서비스 | 중복 요청 자동 감지, 멱등성 보장 |
| **Kafka Consumer Offset 관리** | Logic Service | 수동 커밋으로 "처리 완료 후에만" 오프셋 전진 |
| **Connection Draining** | Gateway 롤링 업데이트 시 | 기존 커넥션 유지 + 신규 커넥션만 새 Pod로 |
| **Backpressure** | MPSC 큐 | max_capacity 초과 시 enqueue 거부 (`QueueError::Full`). 80% 슬로우다운/429 응답은 Gateway 구현 시 추가 예정 |

---

## 6. 데이터 계층 (L1 → L2 → L3)

```
[L1] 프로세스 내 로컬 캐시 (unordered_map, TTL 짧게) ← 네트워크 홉 제로
  ↓ miss
[L2] Redis Cluster (핫 데이터/세션/분산 락) ← ns별 논리 분리
  ↓ miss
[L3] PostgreSQL (영속 데이터) ← schema per service, Repository 추상화로 DBMS 교체 가능

※ L1 캐시 무효화: Redis Pub/Sub 브로드캐스트로 전 인스턴스 동기화
```

```
[Redis namespace]
auth:session:*      auth:token:*       auth:bloom:*
chat:room:*         chat:msg:*
match:queue:*       match:pool:*

[PostgreSQL schema per service]
auth_schema:  users, tokens, sessions
chat_schema:  rooms, messages, members
match_schema: matches, results, ratings
```

---

## 7. 프로젝트 구조

```
apex_pipeline/                        ← 모노레포 루트 (apex_ prefix 통일)
├── apex_core/                        ← 코어 프레임워크 (namespace apex::core)
│   ├── include/apex/core/            ← 공개 헤더
│   ├── src/                          ← 구현
│   ├── config/                       ← 기본 설정 파일 (default.toml)
│   ├── schemas/                      ← FlatBuffers 스키마 (프레임워크 내장)
│   ├── tests/                        ← unit/integration
│   ├── examples/                     ← 프레임워크 사용 예제
│   ├── bin/                          ← 빌드 출력 (echo_server_debug.exe 등)
│   └── CMakeLists.txt
├── apex_services/                    ← MSA 서비스 (각 서비스 독립 Docker 이미지)
│   ├── gateway/                      ← Gateway 서비스
│   ├── auth-svc/                     ← 인증 서비스
│   ├── chat-svc/                     ← 채팅 서비스
│   └── log-svc/                      ← 로그 서비스
├── apex_shared/                      ← 공유 코드 + FlatBuffers 메시지 정의
│   ├── lib/                          ← 공유 C++ 라이브러리
│   └── schemas/                      ← FlatBuffers 메시지 정의 (전 서비스 공유)
├── apex_infra/                       ← 인프라 설정
│   ├── docker/ci.Dockerfile          ← CI + 로컬 Linux 빌드 겸용 Docker 이미지
│   ├── docker-compose.yml            ← 프로파일: 기본(Kafka/Redis/PG) / observability / full
│   ├── postgres/init.sql             ← 서비스별 스키마 초기화
│   ├── prometheus/prometheus.yml     ← Prometheus 설정
│   ├── grafana/provisioning/         ← Grafana 데이터소스 자동 프로비저닝
│   └── k8s/                          ← Helm charts (스캐폴딩만, 내용 미작성)
├── apex_tools/                       ← CLI, 스크립트, git-hooks
│   ├── git-hooks/                    ← pre-commit hook (main 직접 커밋 방지)
│   ├── claude-plugin/                ← auto-review 멀티에이전트 리뷰 플러그인
│   ├── setup-claude-plugin.sh        ← 플러그인 자동 셋업 스크립트
│   ├── session-context.sh            ← SessionStart 프로젝트 컨텍스트 주입 스크립트
│   └── new-service.sh (예정)          ← 서비스 스캐폴딩 스크립트
└── docs/                             ← 전체 프로젝트 문서 (중앙 집중)
    ├── Apex_Pipeline.md              ← 마스터 설계서
    ├── apex_common/                  ← 프로젝트 공통 (plans/progress/review)
    ├── apex_core/                    ← 코어 프레임워크 문서
    ├── apex_infra/                   ← 인프라 문서
    ├── apex_shared/                  ← 공유 라이브러리 문서
    └── apex_tools/                   ← 도구/플러그인 문서
```

---

## 8. 기술 스택

| 카테고리 | 기술 | 라이브러리 | 상태 |
|----------|------|-----------|------|
| Language | C++23 | MSVC 19.44+ / GCC 14+ / Clang 17+ | 현재 사용 중 |
| Networking | Boost.Asio | Boost 1.84+ | 현재 사용 중 |
| WebSocket | Boost.Beast | Boost 1.84+ (Phase 8a에서 vcpkg 추가 예정) | Phase 8a |
| Framework | apex_core | 자체 개발 (ServiceBase CRTP) | 현재 사용 중 |
| Serialization | FlatBuffers | flatbuffers (zero-copy) | 현재 사용 중 |
| Config | TOML | toml++ (header-only) | 현재 사용 중 |
| Logging | spdlog | spdlog + 구조화 JSON | 현재 사용 중 |
| Build | CMake 3.25+ + Ninja | vcpkg (선언형 vcpkg.json) | 현재 사용 중 |
| Test | Google Test | gtest | 현재 사용 중 |
| Benchmark | Google Benchmark | gbenchmark | Phase 8.5 |
| Message Queue | Apache Kafka (KRaft) | librdkafka 2.x | Phase 6 |
| Cache | Redis 7+ | redis-plus-plus | Phase 7 |
| DBMS | PostgreSQL 16+ | libpq (Asio 어댑터 직접 구현) | Phase 7 |
| Monitoring | Prometheus + Grafana | prometheus-cpp | Phase 9 |
| Auth | JWT + Bloom Filter | jwt-cpp | Phase 8b |
| Container | Docker | multi-stage build | Phase 9 |
| Orchestration | Kubernetes | Helm charts | Phase 9~10 |

---

## 9. apex_core 프레임워크 핵심 설계

### ServiceBase CRTP 패턴

프레임워크가 실행 흐름을 소유하고, 서비스는 핸들러만 등록하는 구조:

```cpp
class EchoService : public ServiceBase<EchoService> {
public:
    EchoService() : ServiceBase("echo") {}

    void on_start() override {
        route<apex::messages::EchoRequest>(0x0001, &EchoService::on_echo);
    }

    // 핸들러는 코루틴 — async_send()를 co_await 가능
    awaitable<Result<void>> on_echo(SessionPtr session, uint16_t msg_id,
                                    const EchoRequest* req) {
        // FlatBuffers zero-copy 읽기 + 응답 빌드 + 비동기 전송
        flatbuffers::FlatBufferBuilder builder(256);
        // ... 응답 빌드 ...
        co_await session->async_send(header, payload);
        co_return ok();
    }
};

int main() {
    // Server::run()이 io_context/스레드를 내부 소유 (프레임워크 모델)
    // 코어별 독립 EchoService 인스턴스 자동 생성 (shared-nothing)
    Server({.port = 9000, .num_cores = 4})
        .add_service<EchoService>()
        .run();  // SIGINT/SIGTERM으로 graceful shutdown
}
```

### 와이어 프로토콜

```
[고정 헤더 10바이트]
ver(u8) | msg_id(u16) | body_size(u32) | flags(u16) | reserved(u8)
[페이로드]
FlatBuffers 바이너리 (zero-copy 접근)
```

### 에러 핸들링 (하이브리드)

| 경로 | 전략 |
|------|------|
| 핫패스 (메시지 수신/파싱/디스패치) | `Result<T> = std::expected<T, ErrorCode>` (zero-cost happy path) |
| 콜드패스 (초기화, 설정 오류) | 예외 throw (`std::out_of_range`, `std::invalid_argument` 등) |
| 복구 불가능 (메모리 고갈) | `std::bad_alloc` throw → 호출자 처리 위임 |

핸들러 반환: `awaitable<Result<void>>` — 코루틴 내에서 `co_return ok()` 또는 `co_return error(ErrorCode::X)`.
디스패치 에러(UnknownMessage, HandlerFailed)와 핸들러 에러(ErrorCode)는 이중 expected로 구분:
`awaitable<std::expected<Result<void>, DispatchError>>` — 외부는 DispatchError, 내부는 Result<void> (= std::expected<void, ErrorCode>).
클라이언트에 ErrorResponse 전송.

### 세션 관리

- 코어 로컬 해시맵 + Redis 백업
- TimingWheel O(1) 하트비트 타임아웃
- shared_ptr 기반 세션 수명 (코루틴 안전성)
- `async_send()` / `async_send_raw()` 비동기 전송 (co_await 가능)

### 코어 엔진 (CoreEngine)

io_context-per-core 아키텍처를 구현하는 핵심 인프라:

- N개 코어 스레드 관리 (1 코어 = 1 io_context = 1 스레드)
- 코어별 MPSC inbox로 코어 간 메시지 전달
- drain_timer 기반 주기적 inbox 소비 + 콜백 실행
- start()/stop()/join() 라이프사이클 + drain_remaining() 정리

### 코어 간 통신

shared-nothing 코어 간 안전한 통신 메커니즘:

| API | 방식 | 용도 |
|-----|------|------|
| `cross_core_call<R>(core_id, F)` | awaitable RPC | 다른 코어에서 함수 실행 후 결과 반환 (타임아웃 지원) |
| `cross_core_post(core_id, F)` | fire-and-forget | 다른 코어에 비동기 작업 전달 (결과 불필요) |

구현: MPSC 큐로 `uint64_t` 메시지 전달 (trivially_copyable 제약), raw new/delete 소유권 모델, CAS 기반 타임아웃/완료 경쟁 해결.

### 보안

- Gateway TLS 종단 (내부 평문)
- JWT 로컬 검증 → 블룸필터 체크 → 필요 시만 Redis 블랙리스트 조회
- 보안 민감 작업은 메시지 타입별 강제 Redis 검증

---

## 10. 개발 진행 상황

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
| 5 | - | 기반 정비: CI/CD + TOML + spdlog + Shutdown 타임아웃 | +16 케이스 |

#### 활성 로드맵 (Phase 6+)

> Phase 5부터 순차 정수. 각 Phase = 하나의 의존성 묶음.
> Phase 간 병렬 관계는 ∥ 표기.

| Phase | 버전 | 내용 | 의존 |
|-------|------|------|------|
| 6 | v0.3.0 | Kafka 체인: Kafka 어댑터 → KafkaSink + trace_id | Phase 5 |
| 7 | v0.3.0 | 데이터 체인: Redis 어댑터 ∥ PG 어댑터 → Connection Pool | Phase 5 |
| 8a | - | WebSocket 프로토콜: ProtocolBase 구현체 (Beast 통합) | Phase 6 + 7 |
| 8b | v0.4.0 | Gateway + Auth: TLS, JWT, 블룸필터, Kafka 라우팅, Rate Limiting | Phase 8a |
| 9 | v0.5.0 | 운영 인프라: 메트릭 + Docker + K8s + CI/CD 고도화 | Phase 8b |
| 10 | v1.0.0 | 최종 통합: E2E 검증 + 부하 테스트 + 문서 정리 | Phase 9 |

> Phase 6 ∥ Phase 7 — 서로 독립, 동시 진행 가능. 둘 다 완료 시 v0.3.0 태그.
> Phase 8a → 8b — WebSocket 프로토콜 완성 후 Gateway+Auth 진행.

### 전체 TODO

#### 완료
- [x] apex_core 기반 컴포넌트 ~ v0.2.4 (상세는 완료 이력 참조)

#### Phase 5: 기반 정비
- [x] CI/CD (GitHub Actions — 빌드+단위 테스트, concurrency로 중복 실행 자동 취소)
- [x] TOML 설정 로딩 (toml++)
- [x] spdlog 기본 통합 (ConsoleSink + FileSink, TOML 설정 연동)
- [x] Graceful Shutdown drain 타임아웃 (TOML 설정 가능)

#### apex_tools: auto-review 플러그인 (v1.1)
- [x] 5개 전문 리뷰어 에이전트 병렬 리뷰 (docs/structure/code/test/general)
- [x] 리뷰 → 수정 → 재리뷰 루프 (Clean까지) → PR + CI 자동화
- [x] v1.1: 2-tier 멀티에이전트 (code/test + superpowers:code-reviewer 서브에이전트)
- [x] v1.1: confidence threshold ≥80 → ≥40 하향
- [x] v1.1: Smart Re-review Skip (파일 타입 매핑 + 의미적 영향 판단)
- [x] v1.1: Phase 4 CI 실패 인프라/코드 분기, 대규모 수정 시 재리뷰
- [x] v1.1: Phase 5 자동 squash merge + 워크트리 정리

#### 빌드 환경 개선
- [x] .gitattributes CRLF/LF 정규화
- [x] VCPKG_INSTALLED_DIR 공유 + hostSystemName 빌드 디렉토리 분리
- [x] 빌드 스크립트 리팩토링 (사전 체크 + 버전 검증)
- [x] CI Docker 이미지 (ubuntu:24.04 + GCC 14, CI/로컬 Linux 빌드 겸용)
- [x] CI workflow Docker container 전환 (Linux 잡 apt-get/vcpkg 설치 제거)

#### Phase 6: Kafka 체인
- [ ] Kafka 어댑터 (librdkafka fd → Asio, Producer 공유 + Consumer 분리)
- [ ] KafkaSink (spdlog sink 추가) + trace_id 자동 주입

#### Phase 7: 데이터 체인
- [ ] Redis 어댑터 (redis-plus-plus async)
- [ ] PostgreSQL 어댑터 (libpq → Asio)
- [ ] Connection Pool (코어별 독립, health check)

#### Phase 8a: WebSocket 프로토콜
- [ ] WebSocketProtocol (Boost.Beast, ProtocolBase CRTP 구현체)
- [ ] ping/pong 하트비트 (프로토콜 내장)

#### Phase 8b: Gateway + Auth 서비스
- [ ] Gateway (TLS, JWT, 블룸필터, Kafka 라우팅, Rate Limiting)
- [ ] Auth 서비스 (JWT 발급/검증/블랙리스트)
- [ ] FlatBuffers 공유 메시지 스키마 정의

#### Phase 8.5: 파이프라인 통합
- [ ] E2E 통합 테스트 → **v0.4.0**
- [ ] 코어 프레임워크 baseline 벤치마크 (Google Benchmark — TPS/Latency)

#### Phase 9: 운영 인프라
- [ ] Prometheus 메트릭 노출
- [ ] Docker 멀티스테이지 빌드
- [ ] K8s manifests + Helm
- [ ] CI/CD 고도화 (이미지 빌드 + 배포, docker-compose 통합 테스트)
- [ ] 서비스 스캐폴딩 스크립트

#### Phase 10: 최종 통합
- [ ] K8s E2E 테스트
- [ ] 부하 테스트 시나리오 (목표 TPS/Latency)
- [ ] 모니터링 대시보드 구성

#### 백로그 (해당 Phase에서 흡수)
- [ ] Circuit Breaker (→ Phase 8b)
- [ ] Dead Letter Queue (→ Phase 6)
- [ ] Idempotency Key (→ Phase 8b)
- [ ] Kafka 토픽/파티션 설계 (→ Phase 6 브레인스토밍)
- [ ] Redis 키 네이밍 (→ Phase 7 브레인스토밍)
- [ ] PostgreSQL 스키마 설계 (→ Phase 8b 브레인스토밍)

#### 백로그 — 성능 최적화 (벤치마크 후)
- [ ] CPU Affinity + SO_REUSEPORT
- [ ] 배치 I/O (writev)
- [ ] 코루틴 프레임 풀 할당 (ADR-21)
- [ ] L1 로컬 캐시
- [ ] epoll → io_uring (ADR-15)

---

# Apex Pipeline - Why Document (기술 결정 근거)

모든 아키텍처 결정과 기술 선택에 대한 **정(Thesis) → 반(Antithesis) → 합(Synthesis)** 분석

---

## 1. 왜 MSA인가? (Monolith vs MSA)

**정: MSA를 선택한 이유**
- 각 서비스를 독립 배포/스케일 가능 → Gateway만 10대, Auth는 2대처럼 유연한 자원 배분
- 서비스 장애가 전체로 전파되지 않음 (Chat이 죽어도 Auth는 살아 있음)
- 서비스별 기술 선택 자유 (C++, Python 혼용 가능)

**반: Monolith의 장점도 있다**
- 1인 개발에서 MSA는 오버엔지니어링이라는 비판이 타당함
- 서비스 간 통신 오버헤드 (네트워크 홉 추가)
- 분산 시스템 고유의 복잡성 (분산 트랜잭션, 일관성 문제)

**합: MSA를 선택하되, 과도한 분리는 하지 않는다**
- 서비스 수를 3~4개로 제한 (Gateway, Auth, 핵심 Logic 1~2개)
- 공유 라이브러리(core/ 프레임워크)로 중복 코드 최소화
- 로컬 개발 시 docker-compose로 전체를 한 번에 띄우는 편의성 확보
- **포트폴리오 목적상 MSA 설계 능력을 보여주는 것 자체가 핵심 가치**

---

## 2. 왜 C++인가? (C++ vs Go vs Rust vs Java)

**정: C++을 선택한 이유**
- 메모리 직접 제어 → Object Pool, Memory Pool 등 극한 최적화 가능
- GC 없음 → 예측 가능한 지연 시간 (Java/Go는 GC pause 불가피)
- 7년간 축적된 본인의 핵심 역량 → 생산성 최고
- 게임 서버급 성능이 이 파이프라인의 존재 이유

**반: 다른 언어의 장점**
- **Go**: goroutine 기반 동시성이 매우 쉬움, 빌드 속도 빠름, 클라우드 생태계 1등
- **Rust**: C++ 수준 성능 + 메모리 안전성 보장, 최신 트렌드
- **Java/Kotlin**: 엔터프라이즈 생태계 압도적, Kafka 라이브러리 성숙도 최고

**합: C++이 정답이되, 약점을 인지하고 보완한다**
- C++의 약점(메모리 안전성)은 스마트 포인터 + ASAN/TSAN으로 보완
- 빌드 속도는 ccache + 모듈 분리로 완화
- **"같은 걸 Go로 만들면 10배 쉽지만, C++로 만들어야 10배 빠르다"가 이 프로젝트의 메시지**

---

## 3. 왜 Boost.Asio인가? (Asio vs io_uring vs libuv)

**정: Boost.Asio를 선택한 이유**
- C++ 비동기 네트워킹의 **사실상 표준** (C++23 std::execution의 전신)
- 크로스 플랫폼 (Linux/Windows/macOS)
- 성숙한 생태계 → Boost.Beast(HTTP/WebSocket), 코루틴 연계
- 본인의 게임 서버 경험에서 검증된 기술

**반: 더 빠른 대안이 있다**
- **io_uring**: Linux 5.1+ 전용. 커널 레벨 비동기 I/O로 시스콜 오버헤드 제거. Asio 대비 30~50% 높은 처리량 가능
- **libuv**: Node.js의 코어. 경량이지만 C 라이브러리라 C++ 래핑 필요

**합: Boost.Asio + epoll 기본, io_uring은 CMake 선택 옵션**
- 네트워크 I/O에서 io_uring은 epoll 대비 10~20% 개선에 그침 (극적 차이 아님)
- io_uring의 진짜 강점은 파일 I/O — 이 프레임워크는 네트워크 I/O 중심
- Docker 기본 seccomp 프로파일이 io_uring 시스콜 차단 → 프로덕션 마찰
- BOOST_ASIO_HAS_IO_URING은 컴파일 타임 스위치 → CMake 옵션 `APEX_USE_IO_URING`으로 제어 (기본 OFF)
- **"io_uring의 한계를 알면서 선택하지 않은" 판단이 포트폴리오에서 깊이를 보여줌**

---

## 4. 왜 Kafka 중앙 버스인가? (Kafka only vs gRPC + Kafka)

**정: Kafka 중앙 버스로 통일한 이유**
- 자체 프레임워크(apex_core)가 모든 네트워킹을 담당 → gRPC의 역할이 소멸
- 서비스 간 통신을 Kafka로 통일하면 라우팅 로직이 단순해짐
- Kafka의 내구성 + fan-out + 리플레이가 전 구간에 적용됨

**반: gRPC를 유지했을 때의 장점**
- 즉시 응답(request-response)이 필요한 경우 Kafka 경유보다 지연이 낮음
- gRPC의 강타입 인터페이스(.proto)로 서비스 간 계약 명확
- 업계 표준 RPC 프레임워크로 포트폴리오 어필

**합: gRPC를 제거하고, 즉시 응답이 필요한 부분은 Gateway 로컬 처리**
- 인증 검증(JWT)은 Gateway가 로컬에서 처리 (네트워크 홉 제로)
- 잔액 조회 등은 Gateway → Redis 직접 조회 (Kafka 경유 불필요)
- 자체 프레임워크 자체가 gRPC 수준의 타입 안전성 제공 (FlatBuffers + route\<T\>)
- **gRPC 의존성 제거로 빌드/배포 복잡도 대폭 감소**

---

## 5. 왜 Kafka인가? (Kafka vs RabbitMQ vs NATS vs Redis Streams)

**정: Kafka를 선택한 이유**
- **내구성**: 디스크에 영속화 → 서비스 장애 시에도 메시지 유실 없음
- **처리량**: 초당 수백만 메시지 처리 가능 (대용량 트래픽 목표에 부합)
- **Consumer Group**: 같은 토픽을 여러 서비스 인스턴스가 자동 분산 소비
- **리플레이**: 오프셋 되감기로 과거 이벤트 재처리 가능 (디버깅/복구에 강력)

**반: 다른 MQ의 장점**
- **RabbitMQ**: 라우팅 패턴이 유연, 저지연 메시지 전달에 강점. 단, 처리량 Kafka 대비 열위
- **NATS**: 초경량, sub-ms 지연, 설정 거의 불필요. 단, 영속성이 기본이 아님
- **Redis Streams**: 이미 Redis를 쓰니 추가 인프라 불필요. 단, Kafka급 처리량/내구성 미달

**합: Kafka가 정답이되, 오버헤드를 인지한다**
- Kafka의 약점은 **최소 지연이 1~5ms** (디스크 영속화 + 배치 때문)
- 이건 비동기 메시지 처리에서는 문제 아님 (즉시 응답은 Gateway 로컬 처리)
- ZooKeeper 의존성 제거를 위해 **KRaft 모드** 사용 (Kafka 3.3+)
- 1인 개발 환경에서는 단일 브로커로 시작, K8s에서 3 브로커로 확장

---

## 6. 왜 FlatBuffers인가? (FlatBuffers vs Protobuf vs JSON vs MsgPack)

**정: FlatBuffers를 선택한 이유**
- **역직렬화 비용 제로**: 버퍼에서 직접 포인터 접근 (zero-copy)
- Protobuf 대비 10~100배 빠른 읽기 성능
- gRPC 제거로 Protobuf 강제 사유 소멸
- 게임업계에서 검증된 기술 (Unity, Unreal 공식 지원)
- 크로스 플랫폼 호환 (little-endian + vtable 오프셋)

**반: Protobuf의 장점도 있다**
- 스키마 기반 하위 호환성 지원이 더 성숙
- 생태계가 더 넓음 (gRPC, 클라우드 서비스 등)
- 문서/커뮤니티가 더 풍부

**합: FlatBuffers 전면 채택, Serializer 추상 레이어는 YAGNI**
- FlatBuffers 필드 추가도 하위 호환 (새 필드는 기본값)
- gRPC를 쓰지 않으므로 Protobuf 강제 이유 없음
- Serializer 추상 레이어는 YAGNI — 교체할 일이 없으면 만들지 않는다
- 와이어 프로토콜 헤더의 ver 필드로 비호환 변경 대응

---

## 7. 왜 PostgreSQL인가? (PostgreSQL vs MySQL)

**정: PostgreSQL로 변경한 이유**
- MVCC 구현이 더 정교 → 높은 동시성에서 성능 우위
- JSON/JSONB 네이티브 지원 → 유연한 스키마 확장
- LISTEN/NOTIFY → 실시간 DB 이벤트 전달
- 최근 업계 트렌드: 신규 프로젝트는 PostgreSQL 선호 추세
- 고가용성 아키텍처 구성이 더 유연

**반: MySQL의 장점**
- 본인 7년간 실무 경험 → 쿼리 튜닝, 인덱스 설계, 운영 노하우 보유
- 게임업계 표준 DBMS
- InnoDB 엔진의 안정성은 충분히 검증됨

**합: PostgreSQL 채택. libpq → Asio 어댑터 직접 구현**
- 고가용성 + 업계 트렌드 + MVCC 우수성으로 PostgreSQL 선택
- Repository 패턴으로 DBMS 추상화 → 교체 가능성 확보
- libpq fd를 Asio에 등록하는 어댑터를 직접 구현 (포트폴리오 가치)
- DB 커넥션 폭증 문제: **DB Proxy로 해결** (PgBouncer/Odyssey/PgCat)
- MySQL 운영 경험은 여전히 유효 — PostgreSQL에도 적용 가능한 범용적 지식

---

## 8. 왜 Redis인가? (Redis vs Memcached vs 자체 인메모리 캐시)

**정: Redis를 선택한 이유**
- 단순 Key-Value 외에 Sorted Set, Hash, Pub/Sub, Stream 등 풍부한 자료구조
- 클러스터 모드로 수평 확장 가능
- Pub/Sub → L1 캐시 무효화 브로드캐스트, 블룸필터 갱신
- 레이트 리밋, 세션, 랭킹, 분산 락 등 **범용 도구**

**반: 대안도 있다**
- **Memcached**: 순수 캐시로는 더 빠름 (멀티스레드 아키텍처). 단, 자료구조 없음
- **자체 인메모리 캐시**: 네트워크 홉 제거로 최고 속도. 단, 분산 불가

**합: Redis + 로컬 L1 캐시 2단 구조**
- 자주 조회되는 핫 데이터는 **로컬 캐시로 Redis 호출도 제거**
- Redis는 분산 환경에서의 일관성 + 풍부한 자료구조 용도로 활용
- 로컬 캐시 무효화는 Redis Pub/Sub로 브로드캐스트

---

## 9. 왜 Docker + K8s인가? (K8s vs Docker Compose vs Bare Metal)

**정: Docker + K8s를 선택한 이유**
- 서비스별 독립 배포의 전제 조건 (MSA = 컨테이너화 필수)
- K8s의 자동 스케일링 (HPA) → 트래픽에 따라 Pod 수 자동 조절
- 롤링 업데이트 → 무중단 배포
- Health Check + 자동 재시작 → 안정성 자동화

**반: 오버엔지니어링 우려**
- 1인 개발에서 K8s 클러스터 관리는 본질(서버 개발)에서 벗어남
- 로컬 개발 시 K8s는 무거움

**합: 개발은 docker-compose, 배포는 K8s**
```
[로컬 개발] docker compose up -d → Kafka, Redis, PostgreSQL 기동 (기본)
[스테이징] minikube 또는 k3s → 경량 K8s로 오케스트레이션 검증
[프로덕션] K8s (클라우드 매니지드: EKS/GKE)
```
- Dockerfile은 **멀티스테이지 빌드**로 이미지 최소화
- Helm Chart로 환경별 설정 분리
- docker-compose 프로파일: 기본(Kafka,Redis,PostgreSQL) / observability(+Prometheus,Grafana) / full(향후)

---

## 10. 왜 Gateway 패턴인가? (Gateway vs Direct 통신)

**정: Gateway를 두는 이유**
- 클라이언트는 **단 하나의 엔드포인트**만 알면 됨 → API 복잡도 은닉
- 인증/레이트리밋/로깅을 **한 곳에서** 처리
- TLS 종단 → Gateway에서만 암호화 처리, 내부 통신은 평문으로 성능 확보
- 프로토콜 변환 → 외부(WS/TCP) ↔ 내부(Kafka) 브릿지

**반: Gateway가 SPOF(단일 장애점)가 될 수 있다**
- 모든 트래픽이 Gateway를 거침 → Gateway 장애 = 전체 서비스 장애

**합: Gateway를 Stateless하게 유지하고, 다중 인스턴스로 SPOF 제거**
- Gateway는 세션 상태를 Redis에 저장 → N대로 자유롭게 스케일아웃
- K8s Service/Ingress가 Gateway 앞단에서 로드밸런싱
- Gateway에는 **라우팅 + 인증 검증 + 레이트리밋만** 두고, 비즈니스 로직은 넣지 않음

---

## 11. 왜 이 안정성 패턴들인가?

### Circuit Breaker
- **왜 필요?** 외부 서비스 호출 시 장애 서비스가 응답하지 않으면 → 연쇄 장애
- **어떻게?** 실패 N회 → 회로 차단 → 즉시 에러 반환 → 반개방(half-open)으로 복구 확인
- **구현:** core/ 프레임워크에 CircuitBreaker 클래스로 공통화

### Dead Letter Queue
- **왜 필요?** Kafka Consumer가 특정 메시지를 반복 실패하면 해당 파티션 전체가 멈춤
- **어떻게?** 재시도 3회 실패 → DLQ 토픽으로 이동 → 정상 메시지는 계속 처리
- **운영:** DLQ 모니터링 + 수동 재처리 도구

### Graceful Shutdown
- **왜 필요?** K8s 롤링 업데이트 시 진행 중 요청이 끊기면 데이터 손실
- **어떻게?** SIGTERM → acceptor 중지 → 코어별 세션 close(코어 스레드에 비동기 post) → 세션 drain 폴링(active_sessions==0 대기, 1ms 주기) → CoreEngine stop → CoreEngine join → drain_remaining(잔여 MPSC 메시지 소비) → 서비스 정지 → 종료
- **K8s:** `terminationGracePeriodSeconds` 30초 대비 기본 25초 (5초 여유) — ADR-05에서 설계 확정, Phase 5에서 구현

### Idempotency Key
- **왜 필요?** 네트워크 타임아웃으로 클라이언트가 재시도 → 중복 처리 위험
- **어떻게?** 클라이언트가 요청마다 UUID 첨부 → Redis에 키 존재 확인 → 중복이면 이전 결과 반환

---

## 정반합 결과: 원본 대비 설계 변경 요약

| # | 변경 사항 | 원본 | 현재 | 근거 |
|---|----------|------|------|------|
| 1 | 서비스 간 RPC | gRPC | **제거** (Kafka 통일) | 자체 프레임워크가 전담, 복잡도 감소 |
| 2 | 직렬화 | Protobuf (기본) + FlatBuffers (핫패스) | **FlatBuffers 전면** | gRPC 제거로 Protobuf 강제 소멸, zero-copy |
| 3 | Serializer 추상화 | 있음 (shared/lib) | **YAGNI 제거** | 교체할 일 없으면 만들지 않는다 |
| 4 | DBMS | MySQL | **PostgreSQL** | 고가용성 + 업계 트렌드 + MVCC |
| 5 | C++ 표준 | C++17 | **C++23** | co_await 코루틴, std::expected, concepts |
| 6 | io_uring | 자동 활성화 | **epoll 기본, 선택적** | Docker seccomp 호환, 네트워크 I/O 개선 미미 |
| 7 | 패키지 관리 | Conan/vcpkg | **vcpkg만** | vcpkg.json 선언형으로 통일 |
| 8 | 프레임워크 | 없음 (각 서비스 개별) | **apex_core** | ServiceBase CRTP, 코드 재사용 극대화 |
