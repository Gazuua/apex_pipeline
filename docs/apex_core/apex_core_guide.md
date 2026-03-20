# apex_core 프레임워크 가이드

**버전**: v0.5.5.2 | **최종 갱신**: 2026-03-19
**목적**: 이 문서 하나만 읽고 apex_core 위에 새 서비스를 올릴 수 있다.

> **설계 결정 D1-D7**: 이 문서에는 현재 코드에 아직 구현되지 않은 "의도된 설계"가 포함되어 있다.
> 해당 항목에는 `[D*]` 태그가 붙어 있으며, 코드 구현은 #48 에이전트가 담당한다.
> 구현 전까지는 현재 코드와 가이드 사이에 차이가 있을 수 있다.

---

## 목차

- [§1. 퀵 레퍼런스](#1-퀵-레퍼런스)
- [§2. Server 설정 & 부트스트랩](#2-server-설정--부트스트랩)
- [§3. 라이프사이클 훅](#3-라이프사이클-훅)
- [§4. 핸들러 & 메시지](#4-핸들러--메시지)
- [§5. 어댑터 접근](#5-어댑터-접근)
- [§6. 메모리 관리](#6-메모리-관리)
- [§7. 유틸리티](#7-유틸리티)
- [§8. 금지사항 & 안티패턴](#8-금지사항--안티패턴)
- [§9. 빌드 시스템 통합](#9-빌드-시스템-통합)
- [§10. 내부 아키텍처](#10-내부-아키텍처)
- [§11. 실전 서비스 패턴](#11-실전-서비스-패턴)

---

## §1. 퀵 레퍼런스

최소 동작 서비스의 전체 코드. 이후 섹션은 이 스켈레톤의 각 부분을 상세히 설명한다.

### TCP 서비스 스켈레톤

```cpp
// ── my_service.hpp ──────────────────────────────────────────────
#pragma once
#include <apex/core/service_base.hpp>

namespace my_app {

class MyService : public apex::core::ServiceBase<MyService> {
public:
    MyService() : ServiceBase("my_svc") {}

    // Phase 1: 어댑터 획득
    void on_configure(apex::core::ConfigureContext& ctx) override {
        // redis_ = &ctx.server.adapter<RedisAdapter>();  // §5 참조
    }

    // Phase 2: 서비스 간 와이어링
    void on_wire(apex::core::WireContext& ctx) override {
        // auto* other = ctx.local_registry.find<OtherService>();  // [D1] §3 참조
    }

    // Phase 3: 핸들러 등록
    void on_start() override {
        route<MyRequest>(0x1000, &MyService::on_request);  // §4 참조
    }

    void on_stop() override {
        // 정리 로직
    }

    void on_session_closed(apex::core::SessionId id) override {
        // per-session 상태 정리
    }

    // ── 핸들러 ──
    boost::asio::awaitable<apex::core::Result<void>>
    on_request(apex::core::SessionPtr session, uint32_t msg_id,
               const MyRequest* req) {
        // ⚠️ req 포인터는 co_await 전까지만 유효 (§8 #1)
        auto data = req->name()->str();  // co_await 전에 복사

        // 응답 빌드 + 전송
        flatbuffers::FlatBufferBuilder fbb(256);
        auto resp = CreateMyResponse(fbb, fbb.CreateString("ok"));
        fbb.Finish(resp);
        session->enqueue_write(
            apex::core::build_frame(msg_id + 1, fbb));  // §4.3 참조

        co_return apex::core::ok();
    }
};

} // namespace my_app

// ── main.cpp ────────────────────────────────────────────────────
#include "my_service.hpp"
#include <apex/core/config.hpp>
#include <apex/core/logging.hpp>
#include <apex/core/server.hpp>
#include <apex/shared/protocols/tcp/tcp_binary_protocol.hpp>
// 어댑터 사용 시: #include <apex/shared/adapters/adapter_base.hpp>
// 어댑터 사용 시: #include <apex/shared/adapters/redis/redis_adapter.hpp> 등

#include <toml++/toml.hpp>

int main(int argc, char* argv[]) {
    // 1. 설정 로드 + 로깅 초기화
    std::string config_path = (argc > 1) ? argv[1] : "my_svc.toml";
    auto app_config = apex::core::AppConfig::from_file(config_path);
    apex::core::init_logging(app_config.logging);  // §2.3 참조

    // 2. Server 구성
    apex::core::Server server({.num_cores = 1});

    server
        .listen<apex::shared::protocols::tcp::TcpBinaryProtocol>(
            9000,
            apex::shared::protocols::tcp::TcpBinaryProtocol::Config{
                .max_frame_size = 64 * 1024
            })
        // .add_adapter<RedisAdapter>(redis_config)  // §5 참조
        .add_service<my_app::MyService>();

    // 3. 실행 (블로킹, SIGINT/SIGTERM으로 graceful shutdown)
    server.run();

    apex::core::shutdown_logging();
    return 0;
}
```

### Kafka-only 서비스 스켈레톤

TCP 리스너 없이 Kafka 메시지만 처리하는 서비스. `[D2]` Kafka 자동 배선에 의해 `kafka_route<T>()` 등록만 하면 KafkaDispatchBridge가 자동 생성된다.

```cpp
class MyKafkaService : public apex::core::ServiceBase<MyKafkaService> {
public:
    MyKafkaService(Config cfg) : ServiceBase("my_kafka_svc"), config_(std::move(cfg)) {}

    void on_configure(apex::core::ConfigureContext& ctx) override {
        kafka_ = &ctx.server.adapter<KafkaAdapter>();
    }

    void on_start() override {
        // kafka_route만 등록하면 코어가 자동으로 KafkaDispatchBridge 배선 [D2]
        kafka_route<MyEvent>(0x2000, &MyKafkaService::on_event);
    }

    boost::asio::awaitable<apex::core::Result<void>>
    on_event(const apex::shared::protocols::kafka::MetadataPrefix& meta,
             uint32_t msg_id, const MyEvent* evt) {
        // meta.corr_id, meta.session_id 등 사용 가능
        co_return apex::core::ok();
    }

private:
    KafkaAdapter* kafka_{nullptr};
    Config config_;
};

// main.cpp — TCP 스켈레톤과 동일하되 listen<P>() 호출 없음
// server.add_service<MyKafkaService>(parsed.config);
// server.add_adapter<KafkaAdapter>(parsed.kafka);
// server.run();
```

---

## §2. Server 설정 & 부트스트랩

### §2.1 ServerConfig

```cpp
struct ServerConfig {
    bool tcp_nodelay = true;                          // Nagle 비활성화 (저지연)
    uint32_t num_cores = 1;                           // io_context 수 (0 = hardware_concurrency)
    size_t mpsc_queue_capacity = 65536;               // 코어 간 MPSC 큐 크기
    std::chrono::milliseconds tick_interval{100};     // CoreEngine tick 주기
    uint32_t heartbeat_timeout_ticks = 300;           // 하트비트 미수신 시 세션 종료 (0=비활성)
    size_t recv_buf_capacity = 8192;                  // per-session 수신 버퍼 크기
    size_t timer_wheel_slots = 1024;                  // TimingWheel 슬롯 수
    bool reuseport = false;                           // Linux: per-core SO_REUSEPORT
    bool handle_signals = true;                       // SIGINT/SIGTERM 자동 처리
    std::chrono::seconds drain_timeout{25};            // Graceful Shutdown drain timeout
    std::chrono::milliseconds cross_core_call_timeout{5000};
    std::size_t bump_capacity_bytes = 64 * 1024;      // per-core BumpAllocator (§6)
    std::size_t arena_block_bytes = 4096;              // per-core ArenaAllocator 블록 크기
    std::size_t arena_max_bytes = 1024 * 1024;         // per-core ArenaAllocator 최대
};
```

### §2.2 Server Fluent API

```cpp
apex::core::Server server({.num_cores = 4});

server
    // 프로토콜별 리스너 (다중 가능)
    .listen<TcpBinaryProtocol>(9000, TcpBinaryProtocol::Config{.max_frame_size = 64*1024})

    // 어댑터 등록 (role 기반 다중 인스턴스)
    .add_adapter<KafkaAdapter>(kafka_config)
    .add_adapter<RedisAdapter>("data", redis_data_config)    // role = "data"
    .add_adapter<RedisAdapter>("pubsub", redis_pubsub_config) // role = "pubsub"
    .add_adapter<PgAdapter>(pg_config)

    // 서비스 등록 — 코어당 1 인스턴스 자동 생성
    .add_service<MyService>(my_config);       // args가 복사되어 각 코어에 전달
    // 또는 팩토리 (PerCoreState 접근 필요 시):
    // .add_service_factory([](PerCoreState& state) { return std::make_unique<MyService>(); })

server.run();  // 블로킹. SIGINT/SIGTERM으로 graceful shutdown
```

**`add_service<T>(args...)` vs `add_service_factory(fn)`**: args가 복사 가능하면 `add_service`, move-only 인자나 PerCoreState 접근이 필요하면 `add_service_factory` 사용.

**`server.global<T>(factory)` `[D3]`**: cross-core 공유 자원을 Server 레벨에서 관리. 최초 호출 시 factory 실행, 이후 동일 `T`에 대해 같은 인스턴스 반환. raw pointer로 참조. §8 #7 참조.

### §2.3 TOML 설정 & 로깅

모든 서비스는 TOML 설정 파일을 사용한다. 프레임워크가 `AppConfig`를 제공:

```cpp
// config.hpp
struct AppConfig {
    ServerConfig server;
    LogConfig logging;
    static AppConfig from_file(const std::string& path);  // TOML 파싱
    static AppConfig defaults();
};

struct LogConfig {
    std::string level = "info";              // app 로거 레벨
    std::string framework_level = "info";    // apex 프레임워크 로거 레벨
    std::string pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [%n] %v";
    std::string service_name;                // 빈 값 → "default"
    LogConsoleConfig console;                // .enabled = true
    LogFileConfig file;                      // .enabled, .path, .json
    LogAsyncConfig async;                    // .queue_size = 8192
};
```

**TOML 구조 예시** (`my_svc.toml`):

```toml
[logging]
level = "debug"
framework_level = "info"
service_name = "my_svc"
[logging.file]
enabled = true
path = ""  # 빈 값 → logs/ 하위에 서비스명/레벨별/날짜별 자동 생성
[logging.async]
queue_size = 8192

[kafka]
brokers = "localhost:9092"
consumer_group = "my-svc"
request_topic = "my.requests"
response_topic = "my.responses"

[redis]
host = "localhost"
port = 6380

[pg]
connection_string = "host=localhost port=5432 dbname=apex_db user=apex_user password=apex_pass"
pool_size_per_core = 2
```

**main()에서의 TOML 파싱 패턴** (Auth Service 참고):

```cpp
auto tbl = toml::parse_file(config_path);
ParsedConfig cfg;
if (auto kafka = tbl["kafka"]; kafka) {
    cfg.kafka.brokers = kafka["brokers"].value_or(std::string{"localhost:9092"});
    cfg.kafka.consumer_group = kafka["consumer_group"].value_or(std::string{"my-svc"});
    // ...
}
```

### §2.4 Kafka 자동 배선 `[D2]`

`kafka_route<T>()`를 1개 이상 등록한 서비스가 있으면, 코어가 `has_kafka_handlers()` 를 감지하여 KafkaDispatchBridge를 자동 생성한다. 서비스 개발자는 핸들러 등록만 하면 된다.

> **현재 코드**: Auth/Chat은 `post_init_callback`에서 수동 배선. `[D2]` 구현 후 이 보일러플레이트는 제거된다.

---

## §3. 라이프사이클 훅

### Phase 개요

| Phase | 훅 | Context | 제공되는 것 | 아직 없는 것 | 전형적 용도 |
|-------|------|---------|------------|------------|-----------|
| 1 | `on_configure` | `ConfigureContext` | `server` (어댑터), `core_id`, `per_core_state` | **다른 서비스** (ServiceRegistry 미포함) | 어댑터 획득, per-core 초기화 |
| 2 | `on_wire` | `WireContext` | `server`, `core_id`, `local_registry`, `global_registry`, `scheduler` | **코어 스레드** (아직 미시작) | 서비스 간 와이어링, 주기 태스크, cross-core 자원 |
| 3 | `on_start` | — | — | — | **핸들러 등록** (handle, route, kafka_route, set_default_handler) |
| 3.5 | (자동) | — | — | — | default_handler 멀티리스너 동기화 |

**실행 순서**: Adapter init → ServiceFactory(per-core) → Phase 1(전 코어) → Phase 2(전 코어) → Phase 3(전 코어) → Phase 3.5 → CoreEngine::run()

### ConfigureContext (Phase 1)

```cpp
struct ConfigureContext {
    Server& server;           // adapter<T>() 접근 가능
    uint32_t core_id;
    PerCoreState& per_core_state;
};
```

**이 Phase에서 하면 안 되는 것**: `ctx.server`로 다른 서비스 접근 시도 — Phase 1 시점에 다른 서비스가 아직 configure 중일 수 있다. ServiceRegistry는 의도적으로 ConfigureContext에서 제외됨.

### WireContext (Phase 2)

```cpp
struct WireContext {
    Server& server;
    uint32_t core_id;
    ServiceRegistry& local_registry;       // 이 코어의 서비스들
    ServiceRegistryView& global_registry;  // 전 코어 서비스 (읽기 전용)
    PeriodicTaskScheduler& scheduler;
};
```

**서비스 간 와이어링 `[D1]`**:

```cpp
void on_wire(WireContext& ctx) override {
    // 타입세이프 서비스 조회 — dynamic_cast 금지 [D1]
    // find<T>()는 포인터 반환 (미등록 시 nullptr).
    // get<T>()는 참조 반환 (미등록 시 std::logic_error 예외).
    auto* other = ctx.local_registry.find<OtherService>();
    if (other) { other_svc_ = other; }

    // 주기 태스크 등록
    ctx.scheduler.schedule(std::chrono::milliseconds{1000},
        [this]() { heartbeat(); });

    // Cross-core 공유 자원 [D3]
    // globals_ = &ctx.server.global<MyGlobals>([]{ return MyGlobals{...}; });
}
```

**이 Phase에서 하면 안 되는 것**: 코루틴 실행, async I/O — 코어 스레드가 아직 시작되지 않았다.

### on_start (Phase 3)

핸들러 등록만 수행한다. §4 참조.

### 런타임 훅: on_session_closed

```cpp
void on_session_closed(SessionId id) override {
    // per-session 상태 정리 (인증 상태, 구독 맵 등)
    auth_states_.erase(id);
}
```

SessionManager가 세션 종료 시 모든 서비스에 통지. Phase와 무관하게 런타임에 호출.

### Shutdown 시퀀스

`finalize_shutdown()` 구현 기준 (server.cpp). 순서가 중요 — 의존성 역순으로 정리한다.

```
Signal (SIGINT/SIGTERM)
 → Server::stop()
 → finalize_shutdown()
    1. Listener stop (acceptor 종료, 신규 연결 거부)
    2. Adapter drain (새 요청 거부, 진행 중 요청 완료 대기)
    3. Scheduler stop_all (주기적 타이머 해제 — 서비스 정지 전에 실행)
    4. Service stop (on_stop() 호출, per-session 상태 정리)
    5. [D7] Outstanding 코루틴 drain 대기 (drain_timeout 내, 10ms 폴링)
    6. CoreEngine stop → join → drain_remaining
       (코어 스레드 종료 후 잔여 MPSC 메시지 소비)
    7. Adapter close (flush + 커넥션 정리) + [D3] Globals clear
    → control_io_.stop() → run() 반환
```

**핵심 순서 근거**:
- Step 3(Scheduler) → Step 4(Service): 타이머 콜백이 정지된 서비스를 참조하지 않도록
- Step 5(코루틴 drain) → Step 6(CoreEngine stop): spawn된 코루틴이 완료될 시간 확보
- Step 6(CoreEngine) → Step 7(Adapter close): 코어 스레드의 completion handler가 어댑터 리소스 참조 가능

`on_stop()`에서 해야 할 것:
- per-session 상태 맵 클리어
- 어댑터 raw pointer null 리셋 (dangling 방지)
- cross-core 자원 참조 null 리셋

---

## §4. 핸들러 & 메시지

### §4.1 핸들러 등록 4종

모든 핸들러는 `on_start()`에서 등록한다.

#### handle() — Raw Binary

```cpp
void handle(uint32_t msg_id,
    awaitable<Result<void>> (Derived::*method)(SessionPtr, uint32_t, span<const uint8_t>));
```

용도: 커스텀 바이너리 프로토콜, FlatBuffers가 아닌 페이로드.

```cpp
void on_start() override {
    handle(0x0001, &MyService::on_raw);
}
awaitable<Result<void>> on_raw(SessionPtr session, uint32_t msg_id,
                                std::span<const uint8_t> payload) {
    co_return ok();
}
```

#### route\<T\>() — FlatBuffers Typed (TCP)

```cpp
template <typename FbsType>
void route(uint32_t msg_id,
    awaitable<Result<void>> (Derived::*method)(SessionPtr, uint32_t, const FbsType*));
```

자동 동작: FlatBuffers Verifier 실행 → 실패 시 에러 프레임 자동 전송 + `warn` 로깅 + `ok()` 반환.

```cpp
void on_start() override {
    route<EchoRequest>(0x1000, &MyService::on_echo);
}
awaitable<Result<void>> on_echo(SessionPtr session, uint32_t msg_id,
                                 const EchoRequest* req) {
    auto name = req->name()->str();  // ⚠️ co_await 전에 복사! (§8 #1)
    co_await do_something();         // req는 이 시점 이후 invalid
    co_return ok();
}
```

#### kafka_route\<T\>() — FlatBuffers Typed (Kafka)

```cpp
template <typename FbsType>
void kafka_route(uint32_t msg_id,
    awaitable<Result<void>> (Derived::*method)(const MetadataPrefix&, uint32_t, const FbsType*));
```

MetadataPrefix 필드: `corr_id`, `core_id`, `session_id`, `user_id`, `source_id`.

```cpp
void on_start() override {
    kafka_route<LoginRequest>(1000, &AuthService::on_login);
}
awaitable<Result<void>> on_login(const MetadataPrefix& meta, uint32_t msg_id,
                                  const LoginRequest* req) {
    // meta.corr_id로 응답 상관관계, meta.session_id로 클라이언트 식별
    co_return ok();
}
```

#### set_default_handler() — Catch-All

```cpp
void set_default_handler(
    awaitable<Result<void>> (Derived::*method)(SessionPtr, uint32_t, span<const uint8_t>));
```

용도: Gateway/프록시 — 등록되지 않은 모든 msg_id를 받는다. Phase 3.5에서 멀티리스너에 동기화.

### §4.2 메시지 정의

**msg_id 할당 규약**:

| 범위 | 용도 | 예시 |
|------|------|------|
| 1-99 | 시스템 메시지 (Gateway 내부) | 3=Authenticate, 4=Subscribe, 5=Unsubscribe |
| 1000-1999 | Auth Service | 1000=LoginReq, 1001=LoginResp, ... |
| 2000-2999 | Chat Service | 2001=CreateRoom, 2002=JoinRoom, ... |
| 3000+ | 추가 서비스 (1000 단위) | — |

**.fbs 파일 배치**:

- 서비스 전용 스키마: `apex_services/<service>/schemas/*.fbs`
- 공유 스키마 (여러 서비스가 참조): `apex_shared/schemas/*.fbs`

**스키마 예시**:

```flatbuffers
namespace apex.auth.fbs;
table LoginRequest {
    email: string;
    password: string;
}
root_type LoginRequest;
```

### §4.3 와이어 프로토콜

**WireHeader** (12바이트, big-endian):

```
[0]       version    (uint8_t, 현재 2)
[1]       flags      (uint8_t)
[2..5]    msg_id     (uint32_t)
[6..9]    body_size  (uint32_t)
[10..11]  reserved   (uint16_t, 0)
```

flags: `NONE=0x00`, `COMPRESSED=0x01`, `HEARTBEAT=0x02`, `ERROR_RESPONSE=0x04`, `REQUIRE_AUTH_CHECK=0x08`

**응답 전송 API 선택**:

| API | 용도 | 비고 |
|-----|------|------|
| `session->enqueue_write(vector<uint8_t>)` | 일반 응답 | write queue에 적재, pump가 순서대로 전송 |
| `session->async_send(WireHeader, span)` | 단일 프레임 직접 전송 | write queue 우회 — 동시 사용 금지 |
| `ErrorSender::build_error_frame(msg_id, code)` | 에러 프레임 빌드 | `enqueue_write`와 조합 |

**Kafka 서비스의 응답 전송** (`send_response` 패턴, D5):

```cpp
void send_response(uint32_t resp_msg_id, uint64_t corr_id, uint16_t core_id,
                   uint64_t session_id, std::span<const uint8_t> payload,
                   std::span<const uint8_t> extra) {
    // KafkaEnvelope 빌드 → Kafka produce (response topic)
    // Gateway의 ResponseDispatcher가 수신 → 해당 세션에 전달
}
```

각 서비스가 자체 `send_response()` 헬퍼를 작성한다 `[D5]`. 공통 패턴은 §11 참조.

---

## §5. 어댑터 접근

### 획득 패턴

`on_configure()`에서 레퍼런스로 획득, 멤버에 raw pointer로 보관:

```cpp
void on_configure(ConfigureContext& ctx) override {
    kafka_ = &ctx.server.adapter<KafkaAdapter>();
    redis_ = &ctx.server.adapter<RedisAdapter>("data");      // role 지정
    redis_pubsub_ = &ctx.server.adapter<RedisAdapter>("pubsub");
    pg_ = &ctx.server.adapter<PgAdapter>();
}
```

### 현재 어댑터 3종

| 어댑터 | Config 타입 | 주요 필드 |
|--------|-----------|----------|
| `KafkaAdapter` | `KafkaConfig` | `brokers`, `consumer_group`, `consume_topics`, `producer_poll_interval_ms`, `flush_timeout_ms` |
| `RedisAdapter` | `RedisConfig` | `host`, `port`, `password`, `db`, `max_pending_commands` |
| `PgAdapter` | `PgAdapterConfig` | `connection_string`, `pool_size_per_core` |

### 어댑터 에러 처리

```cpp
auto result = co_await redis_->command("GET %s", key.c_str());
if (!result.has_value()) {
    // ErrorCode::CircuitOpen — 서킷 브레이커 열림, 재시도 불필요
    // ErrorCode::PoolExhausted — PG 풀 고갈
    // ErrorCode::ConnectionError — 일시적, 재시도 가능
    spdlog::error("Redis error: {}", static_cast<int>(result.error()));
    // 에러 응답 전송 후 ok() 반환이 일반 패턴
    co_return ok();
}
```

---

## §6. 메모리 관리

### BumpAllocator — 요청 스코프

| 항목 | 값 |
|------|---|
| 용도 | 단일 요청 내 임시 데이터 |
| 수명 | `reset()` 호출까지 |
| 해제 | 개별 해제 불가, `reset()`으로 O(1) 전체 해제 |
| 설정 | `ServerConfig::bump_capacity_bytes` (기본 64KB) |

```cpp
auto& bump = bump();
auto* buf = static_cast<char*>(bump.allocate(1024));
// ... 사용 ...
bump.reset();  // 전체 해제
```

### ArenaAllocator — 트랜잭션 스코프

| 항목 | 값 |
|------|---|
| 용도 | co_await를 넘는 복수 할당 |
| 수명 | `reset()` 호출까지 |
| 오버플로우 | 새 블록 체이닝 (첫 블록 유지, 나머지 해제) |
| 설정 | `arena_block_bytes` (4KB), `arena_max_bytes` (1MB) |

```cpp
auto& arena = arena();
auto* a = arena.allocate(1000);  // 블록 1
auto* b = arena.allocate(5000);  // 블록 2 (체이닝)
arena.reset();                    // 블록 2 해제, 블록 1 리셋
```

### 판단 기준

- co_await 없는 단일 요청 처리 → `bump()`
- co_await를 넘어 데이터 유지 필요 → `arena()`
- 컨테이너, 가변 크기, 서비스 수명 → 힙 (금지 아님)

### Kafka Consumer 메모리 풀 `[D6]`

Kafka consumer 스레드는 코어 외부이므로 per-core allocator(`bump`/`arena`) 접근 금지. 코어가 consumer 전용 메모리 풀을 제공하며, consumer → 코어 간 데이터 전달에 사용한다.

---

## §7. 유틸리티

### cross_core_call / cross_core_post

```cpp
// 요청-응답 (코루틴, 타임아웃)
auto result = co_await server.cross_core_call(target_core, [&]{
    return some_value;  // 타겟 코어에서 실행
});

// Fire-and-forget (비코루틴)
server.cross_core_post(target_core, [=]{
    // 타겟 코어에서 실행 — 값 캡처만 사용
});
```

**주의**: `cross_core_call`의 func는 **값 캡처만** (참조 캡처 시 타임아웃 레이스에서 dangling). `cross_core_post`도 동일.

**per-core 복제 + 동기화 패턴 `[D4]`**: shared-nothing 원칙에 따라, 공유 데이터는 각 코어에 복제하고 변경 시 `cross_core_post()`로 전파한다. shared_mutex 사용 금지.

```cpp
// 구독 추가 시 모든 코어에 전파
for (uint32_t i = 0; i < server.core_count(); ++i) {
    if (i == core_id()) continue;
    server.cross_core_post(i, [channel, session_id]{
        // 타겟 코어의 로컬 구독 맵에 추가
    });
}
```

### PeriodicTaskScheduler

```cpp
// on_wire()에서 등록
auto handle = ctx.scheduler.schedule(
    std::chrono::milliseconds{5000},
    [this]() { cleanup_expired(); });

// 취소
ctx.scheduler.cancel(handle);
```

단일 스레드(per-core io_context)에서만 사용. 스레드 동기화 없음.

### Session 주요 API

```cpp
session->enqueue_write(std::vector<uint8_t> data);   // write queue 적재
session->enqueue_write_raw(std::span<const uint8_t>); // raw 데이터 적재
session->close();                                      // graceful 종료
session->id();                                         // SessionId
session->core_id();                                    // 소속 코어
session->is_open();                                    // 연결 상태
```

### Result\<T\>

```cpp
using Result = std::expected<T, ErrorCode>;
ok();                           // 성공
error(ErrorCode::SomeError);    // 실패
```

모든 핸들러 반환 타입: `awaitable<Result<void>>`.

### spawn() `[D7]`

서비스에서 백그라운드 코루틴을 실행하는 유일한 방법:

```cpp
// on_start() 또는 핸들러에서
spawn([this]() -> awaitable<void> {
    co_await background_work();
});
// 내부: 카운터 증가 → co_spawn → 완료 시 카운터 감소
// shutdown 시 카운터 0 대기
```

`co_spawn(io_context, ..., detached)` 직접 호출 금지 — io_context가 서비스에 노출되지 않음 (§8 #4).

---

## §8. 금지사항 & 안티패턴

### #1. co_await 후 FlatBuffers 포인터 접근

```cpp
// ❌ BAD — co_await 후 req 포인터 dangling
awaitable<Result<void>> handler(SessionPtr s, uint32_t id, const MyReq* req) {
    co_await redis_->command("SET %s %s", "key", req->name()->c_str());
    //                                           ^^^^^^^^^ DANGLING
    co_return ok();
}

// ✅ GOOD — co_await 전에 필요한 데이터 복사
awaitable<Result<void>> handler(SessionPtr s, uint32_t id, const MyReq* req) {
    auto name = req->name()->str();  // 복사
    co_await redis_->command("SET %s %s", "key", name.c_str());
    co_return ok();
}
```

코루틴 재개 시 수신 버퍼가 이미 다음 프레임으로 덮어씌워져 있다.

### #2. 코어 간 직접 상태 공유 `[D4]`

```cpp
// ❌ BAD — shared_mutex로 전 코어 공유
std::shared_mutex mtx_;
std::unordered_map<std::string, SessionId> channel_map_;

// ✅ GOOD — per-core 복제 + cross_core_post 전파
boost::unordered_flat_map<std::string, SessionId> local_channel_map_;  // per-core
// 변경 시 cross_core_post()로 다른 코어에 전파 (§7)
```

shared-nothing 원칙에 따라 서비스 코드 내 뮤텍스는 금지.

**예외**: 외부 라이브러리가 전용 스레드를 운영하는 경우(예: hiredis dedicated Redis 스레드), 해당 스레드와 서비스 스레드 간 공유 데이터에 한해 `std::mutex` 허용. 현재 `PubSubListener::channels_mutex_`가 이에 해당 — Redis PubSub 전용 스레드와 서비스 스레드 간 채널 구독 목록 공유에 사용.

### #3. Phase 순서 위반

```cpp
// ❌ BAD — on_configure에서 다른 서비스 접근
void on_configure(ConfigureContext& ctx) override {
    auto* other = ???;  // ServiceRegistry가 ConfigureContext에 없음 → 컴파일 에러
}

// ✅ GOOD — on_wire에서 서비스 접근
void on_wire(WireContext& ctx) override {
    auto* other = ctx.local_registry.get<OtherService>();  // [D1]
}
```

### #4. io_context 직접 접근 / co_spawn(detached) `[D7]`

```cpp
// ❌ BAD — io_context 직접 접근, 추적 불가 코루틴
auto& io = server.core_io_context(0);
co_spawn(io, my_coro(), detached);  // shutdown 시 대기 불가

// ✅ GOOD — spawn() tracked API 사용
spawn([this]() -> awaitable<void> { co_await my_coro(); });
```

io_context는 서비스에 노출되지 않으므로 직접 `co_spawn` 불가.

### #5. 핸들러 내 동기 블로킹

```cpp
// ❌ BAD — 코어 스레드 전체 블로킹
std::this_thread::sleep_for(std::chrono::seconds{1});

// ✅ GOOD — 비동기 타이머 사용
boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
timer.expires_after(std::chrono::seconds{1});
co_await timer.async_wait(boost::asio::use_awaitable);
```

### #6. 핸들러 error 반환의 의미

```cpp
// ⚠️ error() 반환 시 dispatch가 에러 프레임을 자동 전송함
// 대부분의 경우 직접 에러 응답을 보내고 ok()를 반환하는 게 올바른 패턴

// ✅ GOOD — 직접 에러 응답 + ok() 반환
session->enqueue_write(ErrorSender::build_error_frame(msg_id, ErrorCode::InvalidMessage));
co_return ok();
```

### #7. cross-core 자원에 shared_ptr 사용 `[D3]`

```cpp
// ❌ BAD — shared_ptr 캡처로 소멸 순서 불확정
auto globals = std::make_shared<MyGlobals>();
kafka_.set_callback([globals](auto...) { globals->dispatch(...); });
// globals의 수명이 람다에 의존 → shutdown 소멸 순서 파괴

// ✅ GOOD — server.global<T>() + raw pointer
void on_wire(WireContext& ctx) override {
    globals_ = &ctx.server.global<MyGlobals>([]{ return MyGlobals{...}; });
}
// Server가 소유, raw ptr로 참조. 소멸 순서 명확.
```

---

## §9. 빌드 시스템 통합

### 디렉토리 구조

```
apex_services/my-svc/
├── CMakeLists.txt
├── include/apex/my_svc/
│   └── my_service.hpp
├── src/
│   ├── main.cpp
│   └── my_service.cpp
├── schemas/
│   ├── my_request.fbs
│   └── my_response.fbs
└── tests/
    └── CMakeLists.txt
```

### CMakeLists.txt 템플릿

```cmake
# apex_services/my-svc/CMakeLists.txt

find_package(spdlog CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

# --- FlatBuffers 스키마 컴파일 ---
set(MY_SVC_SCHEMAS
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/my_request.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/my_response.fbs
)
set(MY_SVC_FBS_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${MY_SVC_FBS_DIR})

foreach(SCHEMA ${MY_SVC_SCHEMAS})
    get_filename_component(NAME ${SCHEMA} NAME_WE)
    set(HDR ${MY_SVC_FBS_DIR}/${NAME}_generated.h)
    add_custom_command(
        OUTPUT ${HDR}
        COMMAND flatbuffers::flatc --cpp -o ${MY_SVC_FBS_DIR} ${SCHEMA}
        DEPENDS ${SCHEMA}
        COMMENT "FlatBuffers (my-svc): ${NAME}.fbs"
    )
    list(APPEND MY_SVC_FBS_HEADERS ${HDR})
endforeach()
add_custom_target(my_svc_fbs_generate DEPENDS ${MY_SVC_FBS_HEADERS})

# --- Library ---
add_library(apex_my_svc STATIC src/my_service.cpp)
add_dependencies(apex_my_svc my_svc_fbs_generate)

target_include_directories(apex_my_svc
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${MY_SVC_FBS_DIR}
)

target_link_libraries(apex_my_svc
    PUBLIC
        apex_core
        apex_shared
        apex_shared_adapters_kafka    # 사용하는 어댑터만
        apex_shared_adapters_redis
        apex_shared_adapters_pg
        apex_protocols_kafka
        spdlog::spdlog
    PRIVATE
        flatbuffers::flatbuffers
)
target_compile_features(apex_my_svc PUBLIC cxx_std_23)

# --- Executable ---
add_executable(my_svc_main src/main.cpp)
target_link_libraries(my_svc_main PRIVATE apex_my_svc tomlplusplus::tomlplusplus)

# --- Platform ---
if(WIN32)
    target_compile_options(apex_my_svc PUBLIC /utf-8)
    target_compile_definitions(apex_my_svc PRIVATE _WIN32_WINNT=0x0A00)
    target_compile_options(my_svc_main PRIVATE /utf-8)
    target_compile_definitions(my_svc_main PRIVATE _WIN32_WINNT=0x0A00)
endif()
```

**`apex_services/CMakeLists.txt`에 추가**:

```cmake
add_subdirectory(my-svc)
```

---

## §10. 내부 아키텍처

> 이 섹션은 "어떻게 동작하는가"의 구조 요약. "왜 이렇게 설계했는가"는 ADR 참조 (§10.5).

### §10.1 컴포넌트 배치도

```
Server
 ├── GlobalResourceRegistry [D3]      ← server.global<T>() 자원 소유
 ├── CoreEngine                       ← 스레드 풀, MPSC inbox 드레인
 │    └── PerCoreState[] (코어별 독립)
 │         ├── SessionManager         ← intrusive_ptr<Session> 소유
 │         ├── ServiceRegistry [D1]   ← 타입 기반 서비스 조회
 │         ├── MessageDispatcher      ← msg_id → handler 라우팅
 │         ├── BumpAllocator          ← 요청 스코프 할당
 │         ├── ArenaAllocator         ← 트랜잭션 스코프 할당
 │         ├── PeriodicTaskScheduler  ← 주기 태스크
 │         └── services[]             ← ServiceBaseInterface 인스턴스
 ├── Listener<P>[]                    ← 프로토콜별 (TCP, WebSocket 등)
 │    └── ConnectionHandler<P>[]      ← 코어별, read_loop + frame dispatch
 └── Adapters[]                       ← Kafka, Redis, PG (역할별 다중)
```

### §10.2 Phase 시퀀스

```
Server::run()
 │
 ├── 1. Adapter::init(CoreEngine)     ← 어댑터 초기화
 ├── 2. ServiceFactory per-core       ← 서비스 인스턴스 생성
 ├── 3. [D1] Registry 자동 등록        ← 전 서비스를 ServiceRegistry에
 ├── 4. Phase 1: internal_configure   ← per_core_ 바인딩 + on_configure
 ├── 5. Phase 2: internal_wire        ← on_wire (registry 접근 가능)
 ├── 6. Phase 3: start()              ← on_start (핸들러 등록)
 ├── 7. Phase 3.5: default_handler sync ← 멀티리스너 동기화
 ├── 8. [D2] Kafka 자동 배선           ← has_kafka_handlers() 감지
 ├── 9. post_init_callback (있으면)    ← escape hatch
 └── 10. CoreEngine::run()             ← 코어 스레드 시작
```

### §10.3 TCP 요청 처리 흐름

```
Client → TCP → Session(recv_buffer)
                  ↓
         ConnectionHandler<P>::read_loop()
                  ↓
         P::try_decode(RingBuffer) → Frame
                  ↓
         MessageDispatcher::dispatch(msg_id, session, payload)
                  ↓
         Handler 코루틴 (route<T> / handle / default_handler)
                  ↓
         session->enqueue_write(response) → write_pump → Client
```

### §10.4 Kafka 메시지 흐름

```
[수신]
KafkaAdapter (consumer 스레드)
  → set_message_callback
  → [D6] consumer 메모리 풀에서 payload 복사
  → [D7] spawn() → KafkaDispatchBridge::dispatch(payload)
  → MetadataPrefix 파싱 → kafka_handler_map[msg_id] → Handler 코루틴

[응답 — Kafka-only 서비스]
Handler → send_response() → KafkaAdapter::produce(response_topic, envelope)

[응답 — Gateway 경유]
KafkaAdapter (consumer) → ResponseDispatcher → cross_core_post(session.core_id)
  → session->enqueue_write(response) → Client
```

### §10.5 ADR 포인터 테이블

| 주제 | 위치 | 설명 |
|------|------|------|
| io_context-per-core | `design-decisions.md` § 이벤트 루프 | shared-nothing 아키텍처 근거 |
| CRTP ServiceBase | `design-decisions.md` § 모듈 정체성 | 정적 다형성 선택 이유 |
| MPSC 코어 간 통신 | `design-decisions.md` § 이벤트 루프 | 코어당 수신 큐 1개 설계 |
| 비동기 I/O 통합 | `design-decisions.md` § 비동기 I/O | Kafka/Redis/PG fd를 Asio에 등록 |
| shared-nothing 범위 | `design-decisions.md` § 성능 철학 | 외부 라이브러리 스레드 예외 |
| 코루틴 lifetime | `design-rationale.md` § 코루틴 수명 | intrusive_ptr 캡처, 수명 보장 |
| 메시지 디스패치 | `design-decisions.md` § 메시지 디스패치 | msg_id 기반 flat_map 라우팅 |
| 에러 핸들링 | `design-decisions.md` § 에러 핸들링 | std::expected<T, ErrorCode> |
| 설정 관리 | `design-decisions.md` § 설정 관리 | TOML + hot-reload |
| Protocol concept | `design-rationale.md` § Protocol concept | try_decode/consume_frame 의존성 역전 |

상세: `docs/apex_core/design-decisions.md`, `docs/apex_core/design-rationale.md`

---

## §11. 실전 서비스 패턴

### 패턴 1: Gateway (default_handler + cross-core globals)

**상황**: 모든 클라이언트 메시지를 받아 백엔드로 라우팅하는 프록시.

```cpp
class GatewayService : public ServiceBase<GatewayService> {
    void on_configure(ConfigureContext& ctx) override {
        kafka_ = &ctx.server.adapter<KafkaAdapter>();
    }

    void on_wire(WireContext& ctx) override {
        // cross-core 공유 자원: ResponseDispatcher, BroadcastFanout [D3]
        globals_ = &ctx.server.global<GatewayGlobals>([&]{
            return GatewayGlobals{/*...*/};
        });
        // per-core rate limiter 초기화
    }

    void on_start() override {
        set_default_handler(&GatewayService::on_any_message);
    }

    awaitable<Result<void>> on_any_message(
        SessionPtr session, uint32_t msg_id, span<const uint8_t> payload) {
        // 1. 인증 검사 (per-session 상태)
        // 2. rate limit 검사
        // 3. msg_id 기반 Kafka 토픽 라우팅
        // 4. PendingRequests에 corr_id 등록 (응답 매칭)
        co_return ok();
    }

    void on_session_closed(SessionId id) override {
        auth_states_.erase(id);  // 인증 상태 정리
    }

    GatewayGlobals* globals_{nullptr};
    KafkaAdapter* kafka_{nullptr};
    boost::unordered_flat_map<SessionId, AuthState> auth_states_;
};
```

### 패턴 2: Kafka-only 서비스 (Auth/Chat)

**상황**: TCP 리스너 없이 Kafka 메시지만 처리.

```cpp
class AuthService : public ServiceBase<AuthService> {
    void on_configure(ConfigureContext& ctx) override {
        kafka_ = &ctx.server.adapter<KafkaAdapter>();
        redis_ = &ctx.server.adapter<RedisAdapter>();
        pg_    = &ctx.server.adapter<PgAdapter>();
    }

    void on_start() override {
        // [D2] kafka_route 등록만 하면 자동 배선
        kafka_route<LoginRequest>(1000, &AuthService::on_login);
        kafka_route<LogoutRequest>(1002, &AuthService::on_logout);
        kafka_route<RefreshTokenRequest>(1004, &AuthService::on_refresh);
    }

    awaitable<Result<void>> on_login(
        const MetadataPrefix& meta, uint32_t msg_id, const LoginRequest* req) {
        auto email = req->email()->str();  // co_await 전 복사!
        auto pw = req->password()->str();

        auto pg_result = co_await pg_->query("SELECT ...", {email});
        // ... bcrypt 검증, JWT 생성, Redis 세션 저장 ...

        send_response(1001, meta.corr_id, meta.core_id,
                      meta.session_id, response_payload, {});
        co_return ok();
    }

private:
    // 에러 응답 헬퍼 [D5] — 서비스가 자체 작성
    void send_error(uint32_t msg_id, const MetadataPrefix& meta, uint8_t error_code) {
        flatbuffers::FlatBufferBuilder fbb(128);
        // ... 에러 응답 빌드 + send_response ...
    }
};
```

### 패턴 3: 어댑터 다중 역할

**상황**: 같은 타입의 어댑터를 용도별로 분리.

```cpp
// Server 구성
server
    .add_adapter<RedisAdapter>("data", redis_data_config)     // 데이터 저장
    .add_adapter<RedisAdapter>("pubsub", redis_pubsub_config); // Pub/Sub 전용

// 서비스에서 획득
void on_configure(ConfigureContext& ctx) override {
    redis_data_   = &ctx.server.adapter<RedisAdapter>("data");
    redis_pubsub_ = &ctx.server.adapter<RedisAdapter>("pubsub");
}
```

Chat Service가 이 패턴 사용: "data" (방 멤버십 SET 연산) + "pubsub" (실시간 메시지 PUBLISH).

### 패턴 4: 응답 전송 — TCP direct vs Kafka produce

| 서비스 | 응답 경로 | 이유 |
|--------|----------|------|
| Gateway | `session->enqueue_write()` | 클라이언트와 직접 연결 |
| Auth/Chat | `kafka_->produce(response_topic, envelope)` | Gateway 경유 (직접 세션 없음) |

Gateway의 `ResponseDispatcher`가 Kafka response topic을 구독하여, `corr_id` + `session_id` 기반으로 원래 세션에 응답을 전달한다.
