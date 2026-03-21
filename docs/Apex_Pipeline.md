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
| Service ↔ Redis | Redis 프로토콜 | hiredis (Asio 어댑터 직접 구현) |
| Service ↔ DBMS | PostgreSQL 프로토콜 | libpq (Asio 어댑터 직접 구현) |
| **전 구간 직렬화** | **FlatBuffers** | flatbuffers (zero-copy) |

---

## 4. 속도 최적화 설계

| 기법 | 적용 위치 | 효과 | 상태 |
|------|----------|------|------|
| **io_context-per-core (shared-nothing)** | 전 서비스 | 코어별 독립 이벤트 루프, 락 제거 | ✅ Server 통합 완료 (accept→post()로 코어 이관, SO_REUSEPORT 시 per-core accept) |
| **SPSC All-to-All Mesh** | 코어 간 통신 | 코어 쌍별 전용 SPSC 큐 (N×(N-1)), CAS-free O(1) enqueue | ✅ 구현 |
| **SlabAllocator** | 코어별 독립 | 핫패스 malloc 제거, O(1) 할당 | ✅ 구현 |
| **Zero-copy Ring Buffer** | 수신 버퍼 | memmove 제거, FlatBuffers 직접 접근 | ✅ 구현 |
| **FlatBuffers 직렬화** | 전 구간 | 역직렬화 비용 제로 (zero-copy 읽기) | ✅ 구현 |
| **Connection Pool (PgPool)** | Service → PostgreSQL | 커넥션 생성 비용 제거 | ✅ v0.4.4.0 |
| **Kafka Batch Produce** | Gateway → Kafka | 네트워크 라운드트립 최소화 | ✅ v0.4.1.0 |
| **BumpAllocator** | 코어별 독립 | 요청 수명 임시 데이터, 포인터 전진 O(1) | ✅ v0.4.5.0 |
| **ArenaAllocator** | 코어별 독립 | 트랜잭션 수명, 블록 체이닝 + 벌크 해제 | ✅ v0.4.5.0 |
| **Redis Multiplexer** | Service → Redis | 코어당 고정 커넥션 + 코루틴 파이프라이닝 | ✅ v0.4.5.0 |
| **Cross-Core Message Passing** | 코어 간 통신 | closure shipping 제거, handler dispatch + immutable shared payload | ✅ 구현 |
| **intrusive_ptr Session** | 세션 관리 | shared_ptr atomic 제거, non-atomic refcount + SlabAllocator | ✅ 구현 |
| **SO_REUSEPORT per-core acceptor** | Gateway / Logic Service | 커널 레벨 코어 분배, accept 크로스스레드 제거 | ✅ 구현 |
| **CPU Affinity** | Gateway / Logic Service | 캐시 히트율 극대화 | 백로그 (v1.0.0.0 부하 테스트 후) |
| **L1 로컬 캐시** | 전 서비스 | 프로세스 내 해시맵(TTL) → Redis 호출도 제거 | 백로그 (v1.0.0.0 부하 테스트 후) |
| **코루틴 프레임 풀 할당** | 핫패스 | promise_type operator new → 슬랩 풀 할당 | 백로그 (ADR-21, 벤치마크 후) |
| **배치 I/O (writev)** | 전 서비스 | 시스콜 횟수 최소화 | ✅ 구현 확인 (scatter-gather) |
| **epoll 기본 / io_uring 선택** | I/O 백엔드 | CMake 옵션으로 io_uring 활성화 가능 | ✅ CMake 옵션 구현 |

---

## 5. 안정성 설계

| 패턴 | 적용 위치 | 동작 |
|------|----------|------|
| **Circuit Breaker** | 외부 서비스 호출 | 연속 실패 시 빠른 실패 반환, 연쇄 장애 차단 |
| **Dead Letter Queue** | Kafka Consumer | 처리 실패 메시지를 DLQ 토픽으로 격리, 유실 방지 |
| **Retry + Exponential Backoff** | 모든 외부 호출 | 일시적 실패 자동 재시도, 부하 폭주 방지 |
| **Graceful Shutdown** | 전 서비스 | SIGTERM → Listener stop(acceptor 중지, 코어별 세션 close) → **어댑터 drain (새 요청 거부, is_ready=false)** → **Scheduler stop_all (주기 태스크 중지)** → 서비스 on_stop() → outstanding 코루틴 drain 대기 → CoreEngine stop → CoreEngine join → drain_remaining(잔여 SPSC 메시지 소비) → **어댑터 close (Kafka flush, Redis/PG 풀 close_all)** → globals clear → shutdown_logging() → 종료. 핵심: 어댑터 drain은 서비스 stop **이전** 수행, 어댑터 close는 CoreEngine 종료 **이후** 수행. drain 타임아웃: ADR-05에서 기본값 25초(K8s 30초 대비 5초 여유)로 설계 확정, v0.2.0.0에서 구현. shutdown 후 로깅 시도는 spdlog::get() null 체크로 방어 |
| **Health Check** | K8s Liveness/Readiness | 비정상 Pod 자동 재시작 |
| **Rate Limiting** | Gateway | 3계층: Per-IP Sliding Window (TimingWheel) / Per-User Redis / Per-Endpoint Config |
| **Idempotency Key** | 전 서비스 | 중복 요청 자동 감지, 멱등성 보장 |
| **Kafka Consumer Offset 관리** | Logic Service | 수동 커밋으로 "처리 완료 후에만" 오프셋 전진 |
| **Connection Draining** | Gateway 롤링 업데이트 시 | 기존 커넥션 유지 + 신규 커넥션만 새 Pod로 |
| **Backpressure** | SPSC 큐 | max_capacity 초과 시 enqueue 거부 (`ErrorCode::CrossCoreQueueFull`). co_post_to() awaitable로 비동기 대기 지원. 80% 슬로우다운/429 응답은 Gateway 구현 시 추가 예정 |

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
│   ├── benchmarks/                   ← Google Benchmark (micro/integration)
│   ├── examples/                     ← 프레임워크 사용 예제
│   ├── bin/{variant}/                ← 빌드 출력 (bin/debug/, bin/release/)
│   └── CMakeLists.txt
├── apex_services/                    ← MSA 서비스 (각 서비스 독립 Docker 이미지)
│   ├── gateway/                      ← Gateway 서비스
│   ├── auth-svc/                     ← 인증 서비스
│   ├── chat-svc/                     ← 채팅 서비스
│   ├── log-svc/                      ← 로그 서비스
│   └── tests/                        ← E2E 통합 테스트 + Mock 어댑터
│       ├── e2e/                      ← E2E 시나리오 테스트
│       └── mocks/                    ← Mock 어댑터 인프라 (Kafka/Redis/PG)
├── apex_shared/                      ← 공유 코드 + FlatBuffers 메시지 정의
│   ├── lib/adapters/                 ← 외부 어댑터 라이브러리 (apex::shared::adapters)
│   │   ├── common/                   ← AdapterBase CRTP, PoolLike concept, PoolStats/PoolConfig
│   │   ├── kafka/                    ← KafkaProducer, KafkaConsumer, KafkaAdapter, KafkaSink
│   │   ├── redis/                    ← RedisConnection, RedisMultiplexer, RedisAdapter, HiredisAsioAdapter, RedisReply
│   │   └── pg/                       ← PgConnection, PgResult, PgPool, PgAdapter, PgTransaction
│   ├── lib/protocols/                ← 프로토콜 구현 (apex::shared::protocols)
│   │   ├── tcp/                      ← TcpBinaryProtocol (기존 와이어 프로토콜)
│   │   ├── websocket/                ← WebSocketProtocol (Boost.Beast)
│   │   └── kafka/                    ← KafkaProtocol (Kafka 메시지 프로토콜)
│   ├── tests/                        ← unit/ + integration/ (docker-compose 기반)
│   └── schemas/                      ← FlatBuffers 메시지 정의 (전 서비스 공유)
├── apex_infra/                       ← 인프라 설정
│   ├── docker/ci.Dockerfile          ← CI + 로컬 Linux 빌드 겸용 Docker 이미지
│   ├── docker-compose.yml            ← 프로파일: 기본(Kafka/Redis/PG/PgBouncer) / observability / full
│   ├── pgbouncer/                    ← PgBouncer 설정 (transaction pooling, port 6432)
│   ├── postgres/init.sql             ← 서비스별 스키마 초기화
│   ├── prometheus/prometheus.yml     ← Prometheus 설정
│   ├── grafana/provisioning/         ← Grafana 데이터소스 자동 프로비저닝
│   └── k8s/                          ← Helm charts (스캐폴딩만, 내용 미작성)
├── apex_tools/                       ← CLI, 스크립트, git-hooks, auto-review
│   ├── git-hooks/                    ← pre-commit hook (main 직접 커밋 방지)
│   ├── benchmark/                    ← 벤치마크 PDF 보고서 생성 도구
│   ├── auto-review/                  ← auto-review 설정 (config.md)
│   ├── build-preflight.{bat,sh}      ← 빌드 사전 체크 스크립트
│   ├── claude-plugin/                ← Claude 플러그인 (auto-review 오케스트레이터 + 7개 리뷰어 에이전트)
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
| Hash Map | boost::unordered_flat_map | boost-unordered (vcpkg) | 현재 사용 중 |
| WebSocket | Boost.Beast | Boost 1.84+ | v0.5.0.0 (완료) |
| Framework | apex_core | 자체 개발 (ServiceBase CRTP) | 현재 사용 중 |
| Serialization | FlatBuffers | flatbuffers (zero-copy) | 현재 사용 중 |
| Config | TOML | toml++ (header-only) | 현재 사용 중 |
| Logging | spdlog | spdlog + 구조화 JSON | 현재 사용 중 |
| Build | CMake 3.25+ + Ninja | vcpkg (선언형 vcpkg.json) | 현재 사용 중 |
| Test | Google Test | gtest | 현재 사용 중 |
| Benchmark | Google Benchmark | benchmark (vcpkg) | 현재 사용 중 |
| Message Queue | Apache Kafka (KRaft) | librdkafka 2.x | v0.4.1.0 |
| Cache | Redis 7+ | hiredis + redis-plus-plus (HiredisAsioAdapter 자체 구현) | v0.4.2.0 |
| DBMS | PostgreSQL 16+ | libpq (Asio 어댑터 직접 구현) | v0.4.3.0 |
| Monitoring | Prometheus + Grafana | prometheus-cpp | v0.6.1.0 |
| TLS | OpenSSL | openssl (vcpkg) | v0.5.2.0 (완료) |
| Auth | JWT + Redis 블랙리스트 | jwt-cpp (vcpkg) | v0.5.2.0 (완료) |
| Container | Docker | multi-stage build | v0.6.2.0 |
| Orchestration | Kubernetes | Helm charts | v0.6.3.0 |

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

    // 핸들러는 코루틴 — enqueue_write()로 per-session write queue에 전송
    awaitable<Result<void>> on_echo(SessionPtr session, uint16_t msg_id,
                                    const EchoRequest* req) {
        // FlatBuffers zero-copy 읽기 + 응답 빌드 + 비동기 전송
        flatbuffers::FlatBufferBuilder builder(256);
        // ... 응답 빌드 ...
        session->enqueue_write(apex::core::build_frame(msg_id + 1, builder));
        co_return ok();
    }
};

int main() {
    // Server::run()이 io_context/스레드를 내부 소유 (프레임워크 모델)
    // 코어별 독립 EchoService 인스턴스 자동 생성 (shared-nothing)
    Server server({.num_cores = 4});
    server
        .add_service<EchoService>()
        .listen<TcpBinaryProtocol>(9000)   // TCP 바이너리 프로토콜
        // .listen<WebSocketProtocol>(9001) // WebSocket (v0.5.1+ Beast 통합 후)
        .run();  // SIGINT/SIGTERM으로 graceful shutdown
}
```

### 와이어 프로토콜

```
[고정 헤더 12바이트 (v2)]
ver(u8) | flags(u8) | msg_id(u32) | body_size(u32) | reserved(u16)
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
dispatch 반환: `awaitable<Result<void>>` — `ErrorCode::HandlerNotFound`(미등록), `ErrorCode::HandlerException`(핸들러 예외) 등 모든 에러가 단일 `Result<void>`로 반환.
클라이언트에 ErrorResponse 전송.

### 세션 관리

- 코어 로컬 해시맵 + Redis 백업
- TimingWheel O(1) 하트비트 타임아웃
- intrusive_ptr 기반 세션 수명 (non-atomic refcount, per-core 단일 스레드 보장) + SlabAllocator 할당
- `enqueue_write()` 비동기 전송 — per-session write queue (std::deque) + write_pump 코루틴이 순차 전송. backpressure 지원 (max_queue_depth=256 초과 시 `ErrorCode::BufferFull` 반환)

### 코어 엔진 (CoreEngine)

io_context-per-core 아키텍처를 구현하는 핵심 인프라:

- N개 코어 스레드 관리 (1 코어 = 1 io_context = 1 스레드)
- SPSC all-to-all mesh로 코어 간 메시지 전달 (코어 쌍별 전용 큐, CAS-free)
- event-driven drain: post()+atomic coalescing으로 즉시 소비 (batch limit 1024) + 독립 per-core tick 타이머
- start()/stop()/join() 라이프사이클 + drain_remaining() 정리

### 코어 간 통신

shared-nothing 코어 간 안전한 통신 메커니즘:

| API | 방식 | 용도 |
|-----|------|------|
| `cross_core_call<R>(core_id, F)` | awaitable RPC | 다른 코어에서 함수 실행 후 결과 반환 (타임아웃 지원, 레거시) |
| `cross_core_post(core_id, F)` | fire-and-forget (awaitable) | 다른 코어에 비동기 작업 전달 (레거시) |
| `post_to(core_id, CoreMessage)` | sync | SPSC mesh 직접 전달 (코어 스레드), asio::post fallback (비코어 스레드) |
| `co_post_to(core_id, CoreMessage)` | awaitable | SPSC mesh 전달 + backpressure 대기 (큐 full 시 비동기 재시도) |
| `cross_core_post_msg(engine, src, dst, op, data)` | awaitable | co_post_to 기반 message passing 제로할당 전달 |
| `broadcast_cross_core(engine, src, op, payload)` | awaitable | SharedPayload로 전 코어 전달 (co_post_to 기반) |

구현: CoreMessage(CrossCoreOp + source_core + uintptr_t) → SPSC all-to-all mesh(코어 쌍별 전용 큐) → CrossCoreDispatcher handler dispatch. SharedPayload(atomic refcount)로 immutable 데이터 공유. 코어 스레드에서는 SPSC 큐로 CAS-free 직접 전달, 비코어 스레드(Kafka consumer 등)에서는 asio::post fallback. BroadcastFanout은 asio::post 기반으로 마이그레이션.

### 보안

- Gateway TLS 종단 (내부 평문)
- JWT 로컬 검증 → 블룸필터 체크 → 필요 시만 Redis 블랙리스트 조회
- 보안 민감 작업은 메시지 타입별 강제 Redis 검증

---

## 10. 개발 진행 상황

### 버전 체계

```
v[메이저].[대].[중].[소]

메이저: 0 = 개발 중, 1 = 프레임워크 완성
대:     아키텍처 도메인 전환 시 증가
중:     해당 영역 내 의미 있는 마일스톤
소:     수정, 리뷰, 소규모 개선
각 자리 1~3자리 허용 (v0.12.3.45 가능)
```

### 완료 이력

#### v0.1 — 코어 프레임워크 기초

| 버전 | 규모 | 내용 | 테스트 |
|------|------|------|--------|
| v0.1.0.0 | 대 | 프로젝트 셋업 + 기반 컴포넌트 (MPSC, SlabPool, RingBuffer, TimingWheel) | 29개 |
| v0.1.1.0 | 중 | 코어 프레임워크 통합 (CoreEngine, ServiceBase, WireHeader, FrameCodec) | 누적 69개 |
| v0.1.2.0 | 중 | 프로토콜 + 세션 + Server 통합 + E2E 테스트 | 누적 98개 |
| v0.1.2.1 | 소 | 코루틴 전환 + 코드 리뷰 2회 수정 | 누적 106개 |
| v0.1.3.0 | 중 | ProtocolBase CRTP + 에러 전파 파이프라인 | +18 스위트 |
| v0.1.3.1 | 소 | 코드 리뷰 수정 | +5개 |
| v0.1.4.0 | 중 | Server-CoreEngine 통합, cross_core_call, Graceful Shutdown | +24 케이스 + 리뷰 추가 15건 |

#### v0.2 — 개발 인프라

| 버전 | 규모 | 내용 | 비고 |
|------|------|------|------|
| v0.2.0.0 | 대 | CI/CD + TOML + spdlog + Graceful Shutdown 타임아웃 | +16 케이스 |
| v0.2.0.1 | 소 | TSAN flaky 테스트 수정 + 코드 품질 | PR #4 |
| v0.2.0.2 | 소 | 빌드 환경 개선 (.gitattributes, Docker CI) | PR #6 |
| v0.2.0.3 | 소 | 빌드 스크립트 사전 체크 공용 헬퍼 | PR #9 |

> auto-review 플러그인(v1.1, v1.2)은 개발 도구. 자체 버전으로 관리.

#### v0.3 — 코어 성능 완성

| 버전 | 규모 | 내용 | 비고 |
|------|------|------|------|
| v0.3.0.0 | 대 | 벤치마크 인프라 + 에러 타입 통일 + 핫패스/메모리/구조 최적화 (7 Tier 일괄) | PR #10, +26 파일 |
| v0.3.0.1 | 소 | auto-review 품질 개선: RAII guard(active_sessions_), payload lifetime 문서화, CI permissions 최소 권한, 버전 동기화(0.3.0), 누락 테스트 3건 추가, 에이전트 템플릿 범위 확대 | PR #14, 19 파일 |

Tier 상세:
- Tier 0: Google Benchmark 인프라 (micro 7 + integration 4)
- Tier 0.5: 에러 타입 통일 (DispatchError/QueueError → ErrorCode, Result\<T\>)
- Tier 1: drain/tick 분리, Cross-Core Message Passing, MessageDispatcher unordered_flat_map, zero-copy dispatch
- Tier 1.5: E2E echo 부하 테스터 + JSON before/after 비교 도구
- Tier 2: intrusive_ptr 전환, Session SlabPool 할당, sessions_/timer_to_session_ unordered_flat_map
- Tier 2.5: 벤치마크 시각화 (matplotlib) + PDF 보고서 (ReportLab)
- Tier 3: Server/ConnectionHandler 분리, SO_REUSEPORT per-core acceptor, io_uring CMake 옵션, SlabPool auto-grow

#### v0.4 -- 외부 어댑터

| 버전 | 규모 | 내용 | 비고 |
|------|------|------|------|
| v0.4.1.0 | 중 | Kafka 어댑터: KafkaProducer (전역 공유, SPSC 큐 불필요), KafkaConsumer (Linux io_event_enable / Windows timer polling), KafkaAdapter (AdapterBase CRTP) | +11 단위 테스트 |
| v0.4.1.1 | 소 | KafkaSink: spdlog sink -> Kafka 토픽, JSON 구조화 로그, trace_id MDC 주입 | +4 단위 테스트 |
| v0.4.2.0 | 중 | Redis 어댑터: HiredisAsioAdapter (직접 구현), RedisConnection (코루틴 래퍼), RedisPool (ConnectionPool CRTP), RedisAdapter | +10 단위 테스트 |
| v0.4.3.0 | 중 | PostgreSQL 어댑터: PgConnection (libpq fd -> Asio IOCP/epoll), PgResult (RAII), PgPool (lazy connect), PgAdapter | +10 단위 테스트 |
| v0.4.4.0 | 중 | 공통 추상화 (AdapterBase CRTP, ConnectionPool CRTP, PoolStats), docker-compose PgBouncer, 통합 테스트 CMake 인프라 | +6 단위 테스트, 통합 테스트 4개 (Kafka/Redis/PG/KafkaSink) |
| v0.4.5.0 | 중 | 코어 메모리 아키텍처 (BumpAllocator, ArenaAllocator, SlabPool→SlabAllocator 리네임, CoreAllocator/Freeable/Resettable concepts), Redis 어댑터 개편 (RedisMultiplexer 코루틴 파이프라이닝, RedisReply RAII, ConnectionPool CRTP 제거), PostgreSQL 어댑터 개편 (PgTransaction RAII 가드, PgConnection prepared stmt + BumpAllocator 주입, PgPool CRTP 제거), 어댑터 공통 (PoolLike concept, ConnectionPool CRTP 전면 제거), CoreMetrics + cross_core_post 실패 rate-limited 로깅, ServerConfig TOML 스키마 확장 (메모리 할당기 + cross_core_call_timeout) | +37건 수정 (auto-review 2회) |
| v0.4.5.1 | 소 | Full auto-review v0.4.5.0 (11명 리뷰어, 코드 4건 수정), 문서 타임스탬프 전수 보정 (93건), BACKLOG.md 신설 + 원본 문서 TODO/백로그 전수 제거, CLAUDE.md 압축 분할 (116→54줄), auto-review re_review_scope 스마트 재리뷰 판단 도입, 빌드 무한대기 규칙 + 타임스탬프 date 명령 강제 지침, CI Linux vcpkg binary cache 경로 수정. **Full auto-review v0.4.5.1** (Round 1 Clean, 문서 수정 2건) + 프로세스 개선 3건: start 시그널 타이밍 → coordinator 후발 스폰, 빌드 블로킹 → 메인 책임 이관, 팀 해산 → 메인 전담 해산 | 리뷰+문서 정비+프로세스 개선 |
| v0.4.5.2 | 소 | auto-review 감도 강화 (체크리스트, threshold 50%, cross-domain 관심사) + cross-cutting 리뷰어 신설 → 12명 체제, coordinator 오버랩 정책 + 자동 시작. 코드 리뷰 이슈 6건 수정: PendingCommand UAF (SlabAllocator 전환), silent disconnect 로깅, 어댑터 init 실패 throw, 컨테이너 일관성, RingBuffer shrink, pgbouncer DoS 방어. MIT LICENSE 추가, README 아키텍처 섹션 개편, BACKLOG 갱신. C-2 write queue false positive 드랍. 리뷰 피드백 반영: SessionManager::tick() shrink_to_fit 60초 주기 호출, RedisMultiplexer 2-step ownership transfer 주석 보강, 백로그 해결 완료 항목 정리, clangd vcpkg 인클루드 경로 재추가 | auto-review 강화+코드 수정+리뷰 피드백 |

> 단위 테스트 전수 PASS (auto-review로 추가 보강 중). 통합 테스트는 docker-compose 환경에서 실행 (APEX_BUILD_INTEGRATION_TESTS=ON).

> auto-review v2.0 개편: 3계층 팀 구조(coordinator → auto-review → 11 reviewers) 정립, 리뷰어 자율성 원칙 명시(12 에이전트 파일), full mode 리뷰 이슈 41건 수정(코드 5 + docs 36), CLAUDE.md 빌드 명령어/에이전트 규칙 추가, v0.5 백로그 문서 작성. 프레임워크 버전 변경 없음(도구/문서 개선).

> auto-review v2.1 강화: 리뷰어 감도 체크리스트 + threshold 50% 도입, cross-domain 관심사 확장, cross-cutting 리뷰어 신설(12명 체제), coordinator 오버랩 정책 + 자동 시작 메커니즘.

#### v0.5 — 서비스 체인

| 버전 | 규모 | 내용 | 비고 |
|------|------|------|------|
| v0.5.0.0 | 대 | Wave 1: Protocol concept 기반 의존성 역전 (core=concept, shared=구현), Server 비템플릿 리팩터링 + `listen<P>(port)` 멀티 프로토콜, ListenerBase virtual + ConnectionHandler\<P\> zero-overhead I/O, per-session write queue (std::deque + write_pump), CircuitBreaker + AdapterState 상태 머신, Redis AUTH/ARRAY, 어댑터 retry/reconnect, DLQ, 공유 프로토콜 스키마 4종, WebSocket MVP | PR #25, 단위 테스트 51개 |
| v0.5.2.0 | 중 | Wave 2: Gateway MVP — TLS 종단 (OpenSSL), JWT 검증 (jwt-cpp), msg_id 기반 Kafka 라우팅, TOML hot-reload (FileWatcher), PendingRequests 요청-응답 매칭 | PR #27 |
| v0.5.2.1 | 소 | Wave 2: Rate Limiting 3계층 — Per-IP Sliding Window (TimingWheel), Per-User Redis, Per-Endpoint Config | PR #27 |
| v0.5.3.0 | 중 | Wave 2: Auth Service — JWT 발급/검증/블랙리스트, bcrypt 해싱, Redis 세션 관리, PostgreSQL 사용자 저장소 | PR #27 |
| v0.5.3.1 | 소 | Wave 2: Chat Service — 방 관리, 메시지/귓속말, 히스토리, PubSub 기반 전역 브로드캐스트 | PR #27 |
| v0.5.4.0 | 중 | Wave 2: E2E 통합 테스트 초기 6개 시나리오 (인증 → 채팅 → 브로드캐스트 전체 경로), 이후 v0.5.5.x에서 11개로 확장 | PR #27 |
| v0.5.4.1 | 소 | Wave 2 패치: Mock 어댑터 인프라 (Kafka/Redis/PG) + Gateway/Auth/Chat 단위 테스트 신설, Redis 파라미터 바인딩 API + 인젝션 방어, Gateway 동시성 수정 (ResponseDispatcher core post, Rate Limiter per-core), PubSub WireHeader v2, JWT user_id Kafka 전달, 환경변수 치환, 구독 상한, sleep 제거 → 시간 주입/poll_now 패턴. 56/56 테스트 통과 | PR #28 |
| v0.5.4.2 | 소 | auto-review 21건 수정 | Critical UAF·바이트오더·JWT 5건 + Important 14건 + Minor 2건 | 완료 |
| v0.5.5 | 소 | 서비스 체인 완성: PR #30 리뷰 8건 수정 (kafka_envelope overflow, spdlog 제거, JWT uid string, JwtVerifier copy/move 삭제, config DI, auth exempt TOML 등) + Auth/Chat full impl (MessageDispatcher 기반 핸들러, login/logout/refresh_token, 8개 채팅 핸들러) + E2E 인프라 (RS256 키, fixture launch/teardown, TOML 설정) + 56 테스트 | 완료 |
| v0.5.5.1 | 소 | E2E 인프라 수정 + 서비스 체인 검증 완료: BUILD_TESTING=ON, CTest E2E 제외(-LE e2e). 코어: MessageDispatcher default handler, Server post_init_callback, multi-listener sync_default_handler(Phase 3.5 타이밍 수정). Gateway: TcpBinaryProtocol listen, TOML 구조, GatewayService 배선, ResponseDispatcher, 시스템 메시지. Auth/Chat: CoreEngine 전환, 어댑터 init, bcrypt 시드, PG search_path, response_topic 정합성. DB 스키마: locked_until, token_family. E2E Fixture: 바이너리 경로 주입, 디버그 로그. ASAN UAF 수정(test_redis_adapter 소멸 순서). 71/71 유닛 + 11/11 E2E + CI 전체 통과(ASAN/TSAN 포함) | 완료 |
| v0.5.5.2 | 소 | 로그 디렉토리 구조 확립: async logger + daily_file_format_sink + exact_level_sink 조합. 서비스별/레벨별/날짜별 파일 로깅 구조화, 프로젝트 루트 자동 탐지, service_name TOML 설정 + 검증, E2E 로그 경로 통합. 71/71 유닛 통과 | 완료 |
| v0.5.6.0 | 중 | Post-E2E 코드 리뷰 + 프레임워크 인프라 정비: 10개 관점 체계 리뷰(46건 발견) → 코어 인프라 확장 D2-D7(server.global\<T\>, wire_services 자동 배선, spawn tracked API, ChannelSessionMap per-core shared_mutex 완전 제거, ConsumerPayloadPool, send_error 헬퍼 11개) + post_init_callback 서비스 사용 제거 (프레임워크 API 유지) + GatewayGlobals 소유권 Server 이관 + auto-review 5명(CRITICAL 4+MAJOR 5 추가 수정, 보안 취약점 1건 포함). ~45파일 변경, 71/71 유닛 통과 | 완료 |
| v0.5.7.0 | 중 | 코드 위생 확립: `.clang-format` 도입(Allman brace, 120자) + 전체 274파일 포맷팅 + CI format-check 강제 + `.git-blame-ignore-revs`. `apex_set_warnings()` 정의 + 전 타겟 적용(MSVC `/W4 /WX`, GCC `-Wall -Wextra -Wpedantic -Werror`) + 경고 전수 수정. FileWatcher flaky 테스트 수정. 307파일 변경, 71/71 유닛 + CI 전체 통과 | PR #46 |
| v0.5.8.0 | 중 | CI 파이프라인 확장: build matrix 루트 빌드 통합(apex_shared 검증 포함), UBSAN CMake preset 추가, 서비스 Dockerfile 3개(Gateway/Auth/Chat) + docker-compose.e2e.yml Docker 기반 서비스 기동, E2E CI job(docker compose --wait + ctest -L e2e), Nightly Valgrind workflow(unit+E2E+stress 12개, cron + workflow_dispatch). E2E fixture CreateProcessW → Docker 전환. 71/71 유닛 + CI 전체 통과 | PR #49 |
| v0.5.8.1 | 소 | 백로그 일괄 소탕: CRITICAL 1건(RedisMultiplexer UAF cancelled 플래그), MAJOR 9건(CircuitBreaker HALF_OPEN 인터리빙 + call() 제네릭화, GatewayService set_default_handler 캡슐화, WebSocket msg_id ntohl, ServerConfig 헤더 분리, outstanding_coros_ acq_rel, unordered_flat_map 전환, 문서 4건 갱신), MINOR 3건(safe_parse_u64 Result, #97 부분 해결). Tier 3 아키텍처 이슈 6건 인수인계 문서 작성. 71/71 유닛 통과 | |
| v0.5.8.2 | 소 | Nightly Valgrind 수정 + CI E2E 안정화: valgrind-unit `include(CTest)` + 자체 빌드(DartConfiguration.tcl 생성), valgrind-e2e `gateway_e2e_valgrind.toml`(request_timeout 30s) + 타임아웃 확대 + 3-job 병렬 구조. CI E2E `access_token_ttl_sec` 30→10초 + sleep 31→11초 + recv 기본 타임아웃 10→30초 + `--gtest_filter` 제거(11개 전체 실행) + ServiceRecoveryAfterTimeout flush 루프 제거(boost::asio SO_RCVTIMEO 비호환). 71/71 유닛 + 11/11 E2E + CI 전체 통과 | PR #50 |
| v0.5.8.3 | 소 | Nightly Valgrind 후속 수정: valgrind-unit `MemoryCheckCommand` 절대 경로(`/usr/bin/valgrind`)로 변경, valgrind-e2e `request_timeout_ms` 30s→120s + TCP 대기 300s + Kafka rebalance 60s + gtest_filter 확대(ServiceTimeout/RoomMessageBroadcast/GlobalBroadcast 추가 제외). 잔존 E2E 6개 + Stress 10개 실행 | PR #52 |
| v0.5.8.4 | 소 | Nightly Valgrind 스트레스 테스트 필터 수정: gtest_filter fixture 이름 오류(`StressInfraTest` → `E2EStressInfraFixture` 등) 교정 + Valgrind 감속 하 실패 5개 테스트 추가 제외(MassTimeouts/HalfOpenConnection/DisconnectDuringResponse/ConcurrentRoomJoinLeave). 잔존 Stress 6개 실행 | PR #53 |
| v0.5.9.0 | 중 | Tier 3 아키텍처 정비: SessionId 강타입화(enum class + hash + formatter), core→shared 역방향 의존 해소(forwarding header 제거 + FrameType concept), CoreEngine spawn_tracked + ServiceBase io_context 캡슐화(post/get_executor), ErrorCode 서비스 에러 분리(ServiceError sentinel + GatewayError/AuthError enum). 백로그 6건 해결(CRITICAL 2 + MAJOR 4). 전체 소스 MIT License 저작권 헤더 추가(336파일). branch-handoff.sh 멀티 에이전트 인수인계 시스템. Full Auto-Review v0.5.9.0(37건: CRITICAL 1 connection_handler async_write UB + MAJOR 14 + MINOR 22, 13건 수정). 71/71 유닛 통과 | |
| v0.5.10.0 | 중 | SPSC All-to-All Mesh: CoreEngine MPSC inbox → SPSC all-to-all mesh 전환(N×(N-1) 전용 큐). CAS contention 제거 + cache line bouncing 감소. post_to() 동기 API(SPSC for core threads, asio::post fallback for non-core) + co_post_to() awaitable API(backpressure 지원). cross_core_post_msg/broadcast_cross_core → co_post_to 기반 awaitable 전환. BroadcastFanout asio::post 마이그레이션. CoreEngineConfig mpsc_queue_capacity → spsc_queue_capacity(기본 1024). 신규: spsc_queue.hpp, spsc_mesh.hpp/cpp, core_message.hpp + 단위 테스트 + 벤치마크 | |

### 활성 로드맵

> v0.x = 프레임워크 개발, v1.0.0.0 = 프레임워크 완성 (커스텀 서비스 자유 배포 가능)

```
v0.5.0.0 (완료) ── Wave 1: Protocol concept + 어댑터 회복력
    └──→ v0.5.4.0 (완료) ── Wave 2: 서비스 체인
         v0.5.2.0 Gateway MVP (TLS + JWT + Kafka 라우팅)
         v0.5.2.1 Rate Limiting (3계층: Per-IP / Per-User / Per-Endpoint)
         v0.5.3.0 Auth Service (JWT 발급/검증/블랙리스트, bcrypt, Redis 세션)
         v0.5.3.1 Chat Service (방 관리, 메시지, 브로드캐스트, 히스토리)
         v0.5.4.0 E2E 통합 테스트 (초기 6개 → v0.5.5.x에서 11개 확장)
         v0.5.4.1 Wave 2 패치 (auto-review + 백로그 17건 수정, 56 테스트)
         v0.5.4.2 auto-review 21건 수정 (Critical 5 + Important 14 + Minor 2)
         v0.5.5   서비스 체인 완성 (PR #30 리뷰 8건 + Auth/Chat full impl + E2E 인프라)
         v0.5.5.1 E2E 인프라 수정 + 서비스 체인 검증 완료 (71/71 유닛 + 11/11 E2E, CI ASAN/TSAN 포함 전체 통과)
         v0.5.5.2 로그 디렉토리 구조 확립 (async + daily_file_format + exact_level, 서비스별/레벨별/날짜별 분리)
         v0.5.6.0 Post-E2E 코드 리뷰 + 프레임워크 인프라 정비 (D2-D7 구현, shared_mutex 제거, auto-review)
         v0.5.7.0 코드 위생 확립 (clang-format 전체 적용 + CI 강제, 경고 전수 소탕 + -Werror/WX)
         v0.5.8.0 CI 파이프라인 확장 (루트 빌드 통합, UBSAN, Docker E2E, Nightly Valgrind, 스트레스 12개)
         v0.5.8.1 백로그 일괄 소탕 (CRITICAL 1 + MAJOR 9 + MINOR 3 해결, Tier 3 인수인계)
         v0.5.8.2 Nightly Valgrind 수정 + CI E2E 타이밍 안정화
         v0.5.8.3 Nightly Valgrind 후속 수정 (valgrind-unit 경로 + valgrind-e2e 타임아웃/필터)
         v0.5.8.4 Nightly Valgrind 스트레스 필터 수정 (fixture 이름 교정 + 5개 추가 제외)
         v0.5.9.0 Tier 3 아키텍처 정비 + 저작권 헤더 + Full Auto-Review (37건 발견, 13건 수정)
         v0.5.10.0 SPSC All-to-All Mesh (MPSC→SPSC 전환, co_post_to awaitable, backpressure)
              └──→ v0.6 ── Wave 3: 운영 인프라
                        └──→ v1.0.0.0 — 프레임워크 완성
                                   └──→ v1.1+ — 게임 레퍼런스
```

| 버전 | 규모 | 내용 | 의존 |
|------|------|------|------|
| **v0.4.0.0** | **대** | **외부 어댑터** | |
| v0.4.1.0 | 중 | Kafka 어댑터 (librdkafka fd → Asio, Producer 공유 + Consumer 분리) | v0.3 |
| v0.4.1.1 | 소 | KafkaSink (spdlog sink) + trace_id 자동 주입 | v0.4.1.0 |
| v0.4.2.0 | 중 | Redis 어댑터 (hiredis Asio 직접 통합) | v0.3 |
| v0.4.3.0 | 중 | PostgreSQL 어댑터 (libpq → Asio) | v0.3 |
| v0.4.4.0 | 중 | Connection Pool (코어별 독립, health check) | v0.4.2+3 |
| v0.4.5.0 | 중 | 코어 메모리 아키텍처 + 어댑터 개편 (CRTP→concept, Multiplexer, RAII) | v0.4.4 |
| **v0.5.0.0** | **대** | **서비스 체인** | |
| v0.5.0.1 | 소 | FlatBuffers 공유 메시지 스키마 정의 (Gateway ↔ Auth ↔ Logic 메시지 타입) | v0.3 |
| v0.5.1.0 | 중 | WebSocket 프로토콜 (Boost.Beast, Protocol concept, ping/pong) | v0.3 |
| v0.5.2.0 | 중 | Gateway MVP (TLS 종단, JWT 검증, Kafka 라우팅) | v0.4 + v0.5.1 |
| v0.5.2.1 | 소 | Rate Limiting (3계층: Per-IP Sliding Window / Per-User Redis / Per-Endpoint Config) | v0.5.2 |
| v0.5.3.0 | 중 | Auth 서비스 (JWT 발급/검증/블랙리스트, Redis 세션) | v0.4 + v0.5.2 |
| v0.5.3.1 | 소 | Chat Service (방 관리, 메시지, 브로드캐스트, 히스토리) | v0.4 + v0.5.1 |
| v0.5.4.0 | 중 | 파이프라인 E2E 통합 테스트 | v0.5.2 + v0.5.3 + v0.5.3.1 |
| **v0.6.0.0** | **대** | **운영 인프라** | |
| v0.6.1.0 | 중 | Prometheus 메트릭 노출 | v0.5.4 |
| v0.6.2.0 | 중 | Docker 멀티스테이지 빌드 | v0.5.4 |
| v0.6.3.0 | 중 | K8s manifests + Helm | v0.6.2 |
| v0.6.4.0 | 중 | CI/CD 고도화 (이미지 빌드 + 배포) | v0.6.2 |
| **v1.0.0.0** | **메이저** | **프레임워크 완성** | v0.6 |

v1.0.0.0 포함 항목:
- K8s E2E 테스트 + 부하 테스트 (목표 TPS/Latency)
- 모니터링 대시보드 구성
- 서비스 스캐폴딩 스크립트 + 문서 정리

**v1.0.0.0 완료 기준 (GO/NO-GO 게이트)**

- [ ] 단위 테스트 전수 PASS (ASAN/TSAN clean)
- [ ] docker-compose 전체 시스템 정상 동작 (3종 이상 서비스)
- [ ] K8s (minikube/k3s) 롤링 업데이트 + graceful shutdown 검증
- [ ] 부하 테스트: P99 latency < 100ms, 에러율 < 0.1% (10k TPS 목표)
- [ ] Prometheus + Grafana 기본 메트릭 4종 (처리량, 지연, 에러율, 리소스)
- [ ] 서비스 스캐폴딩 스크립트 (new-service.sh) + 예제 서비스
- [ ] 문서: README + 빌드 가이드 + API 설계 + 운영 매뉴얼

> v0.4.1 (Kafka) ∥ v0.4.2~4 (Data) ∥ v0.5.1 (WebSocket) — 셋 다 v0.3에만 의존, 동시 진행 가능
> v0.5.2 (Gateway) + v0.5.3 (Auth) — v0.4 + v0.5.1 완료 후 진행

### §10.1 작업 묶음 (Work Batches)

로드맵 마일스톤의 실행 단위. 같은 코드 영역 + 같은 아키텍처 결정권 내의 작업을 하나로 묶어 1 브랜치 / 1 PR / auto-review 1회로 처리한다.

| Wave | 묶음 | 포함 작업 | 터치 영역 (에이전트 파일 경계) | 의존 |
|------|------|----------|------------------------------|------|
| **1** | **A. 코드 위생** | clang-tidy 워닝 15건, 테스트 이름 오타, make_socket_pair 순서, ADR-04 stale 갱신, main docs 커밋 squash | `apex_core/src/`, `docs/` | 없음 |
| **1** | **B. 어댑터 회복력** | Redis AUTH, RedisReply ARRAY 지원, PgPool 재시도/백프레셔, CircuitBreaker, KafkaAdapter draining_ 정리, Dead Letter Queue | `apex_shared/` 전체 | 없음 |
| **1** | **C. 프로토콜 + 스키마** | WebSocket 프로토콜, FlatBuffers 메시지 스키마, per-session write queue | `apex_core/include/core/protocol*`, `apex_shared/schemas/` | 없음 |
| **1** | **E-1. 기존 코드 단위 테스트** | ConnectionHandler, PgTransaction, RedisMultiplexer, Session, Server 라이프사이클 단위 테스트 | `tests/` | 없음 |
| **2** | **D. 서비스 체인** | Gateway MVP (TLS+JWT+Kafka라우팅), Rate Limiting 3계층, Auth 서비스, Chat 서비스 | `apex_services/` | Wave 1 |
| **2** | **E-2. E2E 통합 테스트** | Client → Gateway → Auth → Chat → Redis/PG 시나리오 (6개) | `apex_services/tests/e2e/` | Wave 2 D |
| **3** | **F. 운영 인프라** | Prometheus 메트릭, Docker 멀티스테이지, K8s Helm, CI/CD 고도화 | `apex_infra/`, `.github/` | Wave 2 |

**규칙:**
- Wave 내 묶음은 **병렬 실행** (서브에이전트별 파일 경계 준수)
- Wave 간은 **순차 실행** (앞 Wave 완료 + auto-review 통과 후 다음 Wave)
- 빌드는 한 번에 하나만 수행 (동시 빌드 시 시스템 렉)
- 빌드 실패는 작업 프로세스 내 해결, auto-review 진입 전 빌드 성공 필수

### v1.0.0.0 이후 — 게임 레퍼런스

| 버전 | 규모 | 내용 | 의존 |
|------|------|------|------|
| **v1.1.0.0** | **대** | **게임 레퍼런스 구현** | v1.0.0.0 |
| v1.1.1.0 | 중 | 게임 로직 서비스 (매칭, 게임 상태 관리) | v1.0.0.0 |
| v1.1.2.0 | 중 | 게임 클라이언트 (Android/Kotlin) | v1.1.1 |
| v1.1.3.0 | 중 | AWS 배포 + 라이브 데모 | v1.1.2 |

### 전체 TODO

#### 완료
- [x] v0.1 코어 프레임워크 기초 (상세는 완료 이력 참조)

#### v0.2: 개발 인프라
- [x] CI/CD (GitHub Actions — 빌드+단위 테스트, 모든 CI run 완전 병렬 실행)
- [x] TOML 설정 로딩 (toml++)
- [x] spdlog 기본 통합 (ConsoleSink + FileSink, TOML 설정 연동)
- [x] Graceful Shutdown drain 타임아웃 (TOML 설정 가능)

#### v0.3: 코어 성능 완성
- [x] 벤치마크 인프라 (Google Benchmark + micro/integration 벤치마크 + baseline 측정)
- [x] 에러 타입 통일 (DispatchError → ErrorCode 흡수) + async_send → Result\<void\>
- [x] 핫패스 — drain/tick 분리 (post+atomic coalescing + batch limit)
- [x] Cross-Core Message Passing 아키텍처 전환 (closure shipping → op 기반 handler dispatch)
- [x] MessageDispatcher 65536-array → boost::unordered_flat_map
- [x] payload zero-copy dispatch (consume 순서 변경)
- [x] E2E echo 부하 테스터 + JSON before/after 비교 도구
- [x] SessionPtr shared_ptr → intrusive_ptr (non-atomic refcount)
- [x] Session SlabPool 전환 + sessions_/timer_to_session_ → boost::unordered_flat_map
- [x] 벤치마크 시각화 + PDF 보고서 파이프라인
- [x] Server → Server + ConnectionHandler 분리
- [x] SO_REUSEPORT per-core acceptor + io_uring CMake 옵션 + SlabPool auto-grow

#### v0.4: 외부 어댑터
- [x] Kafka 어댑터 (librdkafka fd → Asio, Producer 공유 + Consumer 분리)
- [x] KafkaSink (spdlog sink 추가) + trace_id 자동 주입
- [x] Redis 어댑터 (hiredis Asio 직접 통합, HiredisAsioAdapter 자체 구현)
- [x] PostgreSQL 어댑터 (libpq → Asio, PgBouncer 경유)
- [x] Connection Pool (CRTP 공통 추상화, 코어별 독립, health check, PoolStats)
- [x] 코어 메모리 아키텍처 (BumpAllocator, ArenaAllocator, SlabAllocator 리네임, CoreAllocator/Freeable/Resettable concepts)
- [x] Redis 어댑터 개편 (RedisMultiplexer 코루틴 파이프라이닝, RedisReply RAII, ConnectionPool CRTP 제거)
- [x] PostgreSQL 어댑터 개편 (PgTransaction RAII, PgConnection prepared stmt, PgPool CRTP 제거)
- [x] 어댑터 공통 개편 (PoolLike concept, ConnectionPool CRTP 전면 제거)
- [x] CoreMetrics + ServerConfig TOML 스키마 확장

#### v0.5: 서비스 체인
- [x] WebSocket 프로토콜 (Boost.Beast, Protocol concept, ping/pong) — Wave 1
- [x] FlatBuffers 공유 메시지 스키마 정의 — Wave 2
- [x] Gateway MVP (TLS, JWT, Kafka msg_id 라우팅, TOML hot-reload) — Wave 2
- [x] Rate Limiting (3계층: Per-IP / Per-User Redis / Per-Endpoint) — Wave 2
- [x] Auth 서비스 (JWT 발급/검증/블랙리스트, bcrypt, Redis 세션, PG) — Wave 2
- [x] Chat 서비스 (방 관리, 메시지, 귓속말, 히스토리, 전역 브로드캐스트) — Wave 2
- [x] E2E 통합 테스트 (6개 시나리오: 인증 → 채팅 → 브로드캐스트 전체 경로) — Wave 2

#### v0.6: 운영 인프라
- [ ] Prometheus 메트릭 노출
- [ ] Docker 멀티스테이지 빌드
- [ ] K8s manifests + Helm
- [ ] CI/CD 고도화 (이미지 빌드 + 배포, docker-compose 통합 테스트)

#### v1.0.0.0: 프레임워크 완성
- [ ] K8s E2E 테스트
- [ ] 부하 테스트 시나리오 (목표 TPS/Latency)
- [ ] 모니터링 대시보드 구성
- [ ] 서비스 스캐폴딩 스크립트
- [ ] 문서 정리 + 정합성 최종 검증

#### v1.1.0.0+: 게임 레퍼런스
- [ ] 게임 로직 서비스 (매칭, 게임 상태 관리)
- [ ] 게임 클라이언트 (Android/Kotlin)
- [ ] AWS 배포 + 라이브 데모

#### 백로그 (해당 버전에서 흡수)
- [x] Circuit Breaker (→ v0.5 Wave 1 Phase 1)
- [x] Dead Letter Queue (→ v0.5 Wave 1 Phase 1)
- [ ] Idempotency Key (→ v0.5.2 Gateway)
- [ ] Kafka 토픽/파티션 설계 (→ v0.4 브레인스토밍)
- [x] Redis AUTH (→ v0.5 Wave 1 Phase 1)
- [x] RedisReply ARRAY 지원 (→ v0.5 Wave 1 Phase 1)
- [x] PgPool retry/backpressure (→ v0.5 Wave 1 Phase 1)
- [x] KafkaAdapter AdapterState (→ v0.5 Wave 1 Phase 1)
- [ ] Redis 키 네이밍 (→ v0.4 브레인스토밍)
- [ ] PostgreSQL 스키마 설계 (→ v0.5.3 Auth 브레인스토밍)

#### 백로그 — 성능 최적화 (벤치마크 후)
- [ ] 코루틴 프레임 풀 할당 (ADR-21 — v0.3에서 stack_buf 제거, 풀 할당은 벤치마크 후)
- [ ] L1 로컬 캐시 (v1.0.0.0 부하 테스트 후)
- [x] ~~SO_REUSEPORT~~ → v0.3에서 구현 완료. CPU Affinity는 백로그 유지 (벤치마크 후 판단)
- [x] ~~epoll → io_uring~~ → v0.3으로 흡수
- [x] ~~배치 I/O (writev)~~ → 이미 구현 확인 (session.cpp async_send scatter-gather)

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
- **MSA 설계 능력을 실증하는 것 자체가 프로젝트의 핵심 가치**

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
- **"io_uring의 한계를 알면서 선택하지 않은" 판단이 설계 깊이를 보여줌**

---

## 4. 왜 Kafka 중앙 버스인가? (Kafka only vs gRPC + Kafka)

**정: Kafka 중앙 버스로 통일한 이유**
- 자체 프레임워크(apex_core)가 모든 네트워킹을 담당 → gRPC의 역할이 소멸
- 서비스 간 통신을 Kafka로 통일하면 라우팅 로직이 단순해짐
- Kafka의 내구성 + fan-out + 리플레이가 전 구간에 적용됨

**반: gRPC를 유지했을 때의 장점**
- 즉시 응답(request-response)이 필요한 경우 Kafka 경유보다 지연이 낮음
- gRPC의 강타입 인터페이스(.proto)로 서비스 간 계약 명확
- 업계 표준 RPC 프레임워크로 기술적 어필

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
- libpq fd를 Asio에 등록하는 어댑터를 직접 구현 (기술적 차별점)
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
- **어떻게?** SIGTERM → acceptor 중지 → 코어별 세션 close(코어 스레드에 비동기 post) → 세션 drain 폴링(active_sessions==0 대기, 1ms 주기) → 어댑터 drain(새 요청 거부, is_ready=false) → 서비스 on_stop() → CoreEngine stop → CoreEngine join → drain_remaining(잔여 SPSC 메시지 소비) → 어댑터 close(Kafka flush, Redis/PG 풀 close_all) → shutdown_logging → 종료
- **K8s:** `terminationGracePeriodSeconds` 30초 대비 기본 25초 (5초 여유) — ADR-05에서 설계 확정, v0.2.0.0에서 구현

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
