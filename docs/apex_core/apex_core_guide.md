# apex_core 프레임워크 가이드

**버전**: v0.6.4.0 | **최종 갱신**: 2026-03-26
**목적**: 이 문서 하나만 읽고 apex_core 위에 새 서비스를 올릴 수 있다.

> **설계 결정 D1-D7**: D2-D7은 v0.5.6.0에서 구현 완료. `[D*]` 태그는 설계 결정의 출처 추적용으로 유지한다.
> D1(per-core ServiceRegistry)만 미구현 — 현재 global registry로 동작.

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
        apex::core::WireHeader header{
            .msg_id = msg_id + 1,
            .body_size = static_cast<uint32_t>(fbb.GetSize()),
            .reserved = {}};
        (void)co_await session->async_send(
            header, {fbb.GetBufferPointer(), fbb.GetSize()});

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
    on_event(const apex::core::KafkaMessageMeta& meta,
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
/// Prometheus metrics endpoint configuration (v0.6.1+).
struct MetricsConfig {
    bool enabled = false;       // true로 설정 시 /metrics, /health, /ready HTTP 엔드포인트 노출
    uint16_t port = 8081;       // 메트릭 HTTP 서버 포트
};

struct ServerConfig {
    std::string bind_address = "0.0.0.0";             // 바인드 주소 (예: "127.0.0.1" = 루프백만)
    bool tcp_nodelay = true;                          // Nagle 비활성화 (저지연)
    uint32_t num_cores = 1;                           // io_context 수 (0 = hardware_concurrency)
    size_t spsc_queue_capacity = 1024;                 // 코어 간 SPSC 큐 크기 (per-pair)
    std::chrono::milliseconds tick_interval{100};     // CoreEngine tick 주기
    uint32_t heartbeat_timeout_ticks = 300;           // 하트비트 미수신 시 세션 종료 (0=비활성)
    size_t recv_buf_capacity = 8192;                  // per-session 수신 버퍼 크기
    size_t session_max_queue_depth = 256;             // per-session write queue depth 제한
    size_t timer_wheel_slots = 1024;                  // TimingWheel 슬롯 수
    uint32_t max_connections = 10000;                   // 최대 동시 연결 수 (0 = 무제한, 기본 10000)
    bool reuseport = false;                           // Linux: per-core SO_REUSEPORT
    bool handle_signals = true;                       // SIGINT/SIGTERM 자동 처리
    std::chrono::seconds drain_timeout{25};            // Graceful Shutdown drain timeout
    std::chrono::milliseconds cross_core_call_timeout{5000};
    std::size_t bump_capacity_bytes = 64 * 1024;      // per-core BumpAllocator (§6)
    std::size_t arena_block_bytes = 4096;              // per-core ArenaAllocator 블록 크기
    std::size_t arena_max_bytes = 1024 * 1024;         // per-core ArenaAllocator 최대

    // Metrics (v0.6.1+)
    MetricsConfig metrics;                              // Prometheus 메트릭 엔드포인트 설정
    AdminConfig admin;                                   // Admin HTTP 서버 설정 (런타임 로그 레벨 등)

    // CPU offload
    uint32_t blocking_pool_threads = 2;                  // BlockingTaskExecutor 스레드 수 (§7)
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

### §2.3.1 ScopedLogger (v0.5.11.0+)

`ServiceBase`는 `logger_` (protected) 멤버를 제공한다. `internal_configure()`에서 자동 초기화되며, 서비스 코드에서 직접 사용:

```cpp
// 기본 — [file:line Func] [core=N][MyService] message
logger_.info("handler registered");

// + session — [sess=N] 태그 추가
logger_.debug(session, "user lookup");

// + session + msg_id — [sess=N][msg=0xHHHH] 태그 추가
logger_.warn(session, msg_id, "FlatBuffers verify failed");

// 요청 추적 — [trace=hex] 태그 추가 (copy semantics, 원본 불변)
auto traced = logger_.with_trace(corr_id);
traced.info("routing complete");
```

**로거 분리**: 프레임워크 컴포넌트는 `"apex"` 로거(기본), 서비스는 `"app"` 로거를 사용. `ServiceBase`가 `internal_configure()`에서 자동으로 `"app"` 로거로 초기화하므로 서비스 개발자가 신경 쓸 필요 없다. TOML의 `logging.level`이 app 로거, `logging.framework_level`이 apex 로거를 제어한다.

**프레임워크 외부(비서비스) 사용**: 어댑터·유틸리티 등 `ServiceBase` 밖에서는 직접 생성:

```cpp
// 멤버 변수로 보유 (클래스 수명과 동일)
ScopedLogger logger_{"MyComponent", ScopedLogger::NO_CORE, "app"};

// 함수 스코프 static (namespace-scope static은 금지 — init_logging() 전 생성 시 no-op)
const ScopedLogger& s_logger() {
    static const ScopedLogger instance{"MyUtil", ScopedLogger::NO_CORE};
    return instance;
}
```

### §2.4 Kafka 자동 배선 `[D2]`

`kafka_route<T>()`를 1개 이상 등록한 서비스가 있으면, 코어가 `has_kafka_handlers()` 를 감지하여 KafkaDispatchBridge를 자동 생성한다. 서비스 개발자는 핸들러 등록만 하면 된다.

> **구현 완료** (v0.5.6.0): `wire_services()`가 자동 배선 수행. 각 서비스 main.cpp에서 `post_init_callback` 수동 와이어링이 제거되었다.

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
    4.5. [D7] Outstanding 코루틴 drain 대기 (drain_timeout 내, 10ms 폴링)
         서비스 코루틴 + 인프라 코루틴 + 어댑터 코루틴 모두 대기
    5. Adapter close (flush + 커넥션 정리)
       io_context 아직 실행 중 — close()의 per-core phase가 io_context에 post함
    6. CoreEngine stop → join → drain_remaining
       (코어 스레드 종료 후 잔여 SPSC 메시지 소비)
    7. [D3] Globals clear
    → control_io_.stop() → run() 반환
```

**핵심 순서 근거**:
- Step 3(Scheduler) → Step 4(Service): 타이머 콜백이 정지된 서비스를 참조하지 않도록
- Step 4.5(코루틴 drain) → Step 5(Adapter close): 어댑터 코루틴이 완료된 후에만 close 실행
- Step 5(Adapter close) → Step 6(CoreEngine stop): close()가 io_context에 per-core cleanup을 post하므로, io_context가 실행 중이어야 함 (v0.5.10.6에서 재배치)

`on_stop()`에서 해야 할 것:
- per-session 상태 맵 클리어
- 어댑터 raw pointer null 리셋 (dangling 방지)
- cross-core 자원 참조 null 리셋

### 소멸자 파괴 순서 (v0.5.10.2+)

`finalize_shutdown()`이 미호출된 비정상 경로(예외 등)에서도 UAF가 발생하지 않도록, `~Server()`가 io_context 의존성 역순으로 명시적 파괴한다. C++ 멤버 선언 역순 소멸(RAII)에 의존하지 않는다.

```
~Server()
  1. listeners_.clear()      — acceptor 소켓 정리 (io_context에 등록됨)
  2. scheduler.reset()        — per-core 타이머 정리 (io_context에 등록됨)
  3. core_engine_.reset()     — io_context 소유. ~io_context()가 미완료 코루틴을
                                파괴하며 intrusive_ptr<Session> 해제.
                                per_core_의 slab 메모리가 유효한 상태에서 실행됨.
```

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
    awaitable<Result<void>> (Derived::*method)(const KafkaMessageMeta&, uint32_t, const FbsType*));
```

KafkaMessageMeta 필드: `meta_version`, `core_id`, `corr_id`, `source_id`, `session_id`, `user_id`, `timestamp`.

```cpp
void on_start() override {
    kafka_route<LoginRequest>(1000, &AuthService::on_login);
}
awaitable<Result<void>> on_login(const KafkaMessageMeta& meta, uint32_t msg_id,
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
| `session->enqueue_write(vector<uint8_t>)` | 일반 응답 (fire-and-forget) | write queue에 적재, pump가 순서대로 전송 |
| `session->async_send(WireHeader, span)` | 단일 프레임 awaitable 전송 | write queue 경유, 완료 시 Result 반환 |
| `session->async_send_raw(span)` | 로우 프레임 awaitable 전송 | write queue 경유, 완료 시 Result 반환 |
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
| `KafkaAdapter` | `KafkaConfig` | `brokers`, `consumer_group`, `consume_topics`, `producer_poll_interval_ms`, `flush_timeout_ms`, `security` (KafkaSecurityConfig), `extra_properties` |
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

### cross_core_call / cross_core_post (레거시)

```cpp
// 요청-응답 (코루틴, 타임아웃)
auto result = co_await server.cross_core_call(target_core, [&]{
    return some_value;  // 타겟 코어에서 실행
});

// Fire-and-forget (awaitable, v0.5.10.0+)
co_await server.cross_core_post(target_core, [=]{
    // 타겟 코어에서 실행 — 값 캡처만 사용
});
```

**주의**: `cross_core_call`의 func는 **값 캡처만** (참조 캡처 시 타임아웃 레이스에서 dangling). `cross_core_post`도 동일.

### post_to / co_post_to (v0.5.10.0+)

SPSC all-to-all mesh 기반 크로스코어 메시지 전달 API:

- **`post_to(core_id, CoreMessage)`** — 동기 전달. 코어 스레드에서는 SPSC 큐 직접 enqueue, 비코어 스레드(Kafka consumer 등)에서는 asio::post fallback
- **`co_post_to(core_id, CoreMessage)`** — awaitable. 큐가 full이면 비동기 대기 후 재시도 (backpressure)
- **`cross_core_post_msg`**, **`broadcast_cross_core`** — co_post_to 기반으로 awaitable 전환

```cpp
// 메시지 기반 크로스코어 전달 (awaitable)
co_await cross_core_post_msg(engine, source_core, target_core, op, data);

// 전 코어 브로드캐스트 (awaitable)
co_await broadcast_cross_core(engine, source_core, op, shared_payload);
```

**per-core 복제 + 동기화 패턴 `[D4]`**: shared-nothing 원칙에 따라, 공유 데이터는 각 코어에 복제하고 변경 시 크로스코어 메시지로 전파한다. shared_mutex 사용 금지.

```cpp
// 구독 추가 시 모든 코어에 전파
for (uint32_t i = 0; i < server.core_count(); ++i) {
    if (i == core_id()) continue;
    co_await server.cross_core_post(i, [channel, session_id]{
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
session->enqueue_write(std::vector<uint8_t> data);     // write queue 적재 (fire-and-forget)
session->enqueue_write_raw(std::span<const uint8_t>);  // raw 데이터 적재 (fire-and-forget)
co_await session->async_send(WireHeader, span);        // write queue 경유, 완료 대기 (awaitable)
co_await session->async_send_raw(span);                // write queue 경유, 완료 대기 (awaitable)
session->close();                                       // graceful 종료 (SocketBase::close 위임)
session->id();                                          // SessionId (enum class)
session->core_id();                                     // 소속 코어
session->is_open();                                     // 연결 상태
session->socket();                                      // SocketBase& (virtual, TCP/TLS 투명)
```

**SessionId 강타입**: `enum class SessionId : uint64_t {}` — `corr_id`, `user_id` 등과의 암묵적 변환 차단.
변환 헬퍼: `make_session_id(uint64_t)`, `to_underlying(SessionId)`. `std::hash`, `fmt::formatter` 특수화 제공.

### Result\<T\>

```cpp
using Result = std::expected<T, ErrorCode>;
ok();                           // 성공
error(ErrorCode::SomeError);    // 실패
```

모든 핸들러 반환 타입: `awaitable<Result<void>>`.

#### ErrorCode 체계

| 범위 | 용도 | 예시 |
|------|------|------|
| 0 | 성공 | `Ok` |
| 1-98 | 프레임워크 공통 에러 | `InvalidMessage`, `Timeout`, `HandlerNotFound` |
| 99 | 서비스별 에러 sentinel | `ServiceError` |
| 1000-1999 | 애플리케이션 에러 | `AppError` |
| 2000+ | 어댑터 에러 | `AdapterError`, `PoolExhausted`, `CircuitOpen` |

#### ServiceError sentinel 패턴

`ErrorCode::ServiceError`(= 99)는 "에러 상세는 서비스별 코드에 있다"는 표지판이다.
각 서비스는 자체 에러 enum을 정의하고, 에러 프레임의 `service_error_code` 필드로 전달한다.

```cpp
// 서비스별 에러 enum 정의 (uint16_t 기반)
enum class GatewayError : uint16_t { RouteNotFound = 5, ServiceTimeout = 6, ... };
enum class AuthError    : uint16_t { JwtVerifyFailed = 1, ... };

// 서비스 내부 result 타입 (선택적)
using GatewayResult = std::expected<void, GatewayError>;

// 에러 프레임 빌드 — ErrorCode::ServiceError + service_error_code 조합
session->enqueue_write(
    ErrorSender::build_error_frame(
        msg_id,
        ErrorCode::ServiceError,       // 공통 에러 코드
        "",                             // 메시지 (선택)
        static_cast<uint16_t>(GatewayError::RouteNotFound)  // 서비스별 코드
    ));
```

ErrorResponse FlatBuffers 스키마:
```
table ErrorResponse {
    code:uint16;               // ErrorCode (99 = ServiceError)
    message:string;            // 에러 메시지 (nullable)
    service_error_code:uint16; // 서비스별 에러 코드 (code=99일 때 유효)
}
```

클라이언트는 `code == 99`이면 `service_error_code`를 확인하여 서비스별 에러를 판별한다.

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

### post() / get_executor()

io_context가 서비스에 직접 노출되지 않으므로, 다음 두 메서드로 대체한다:

```cpp
// io_context에 작업을 안전하게 포스트
post([this]() { cleanup_expired(); });

// executor 접근 — timer 생성 등에 사용
boost::asio::steady_timer timer(get_executor());
timer.expires_after(std::chrono::seconds{5});
co_await timer.async_wait(boost::asio::use_awaitable);
```

`post(callable)` — 현재 코어의 io_context에 작업을 게시한다.
`get_executor()` — `boost::asio::any_io_executor`를 반환한다. 타이머, resolver 등 executor가 필요한 Asio 객체 생성에 사용.

둘 다 `on_configure` 이후(internal_configure에서 io_context 바인딩 완료)부터 사용 가능.

### blocking_executor() — CPU-bound 작업 offload

CPU-intensive 작업(bcrypt, JWT 서명 등)을 코어 IO 스레드에서 실행하면 해당 코어의 모든 비동기 작업이 블로킹된다. `blocking_executor()`를 사용하여 별도 thread pool로 offload한다:

```cpp
// ❌ BAD — 코어 스레드에서 bcrypt 250ms 블로킹
auto ok = password_hasher_.verify(password, hash);

// ✅ GOOD — thread pool offload, 코어 스레드 즉시 반환
auto ok = co_await blocking_executor().run([&] {
    return password_hasher_.verify(password, hash);
});
```

**동작 원리**: `run()`은 작업을 thread pool에 post하고 호출자 코루틴을 suspend한다. 작업 완료 후 호출자의 코어 executor로 자동 resume되어 shared-nothing 모델을 유지한다.

**소유**: `Server`가 단일 `BlockingTaskExecutor`를 소유. 스레드 수는 `ServerConfig::blocking_pool_threads` (기본 2).

**Shutdown**: `finalize_shutdown()` Step 6.5에서 `pool_.join()` — 진행 중인 작업 완료 대기 후 종료.

### spawn_tracked() — 인프라 코루틴

`CoreEngine::spawn_tracked(core_id, coro_factory)`는 어댑터 레벨의 인프라 코루틴을 추적한다. 서비스의 `spawn()`과 별도로 관리된다.

```cpp
// KafkaAdapter 내부에서 사용 — 서비스 코드에서는 직접 호출하지 않음
engine.spawn_tracked(0, [bridge, buf]() -> awaitable<void> {
    co_await bridge->dispatch(buf->span());
});

engine.outstanding_infra_coroutines();  // 미완료 인프라 코루틴 수
```

| API | 대상 | 추적 | 호출자 |
|-----|------|------|--------|
| `spawn()` | 서비스 코루틴 | `outstanding_coros_` (서비스별) | 서비스 코드 |
| `spawn_tracked()` | 인프라 코루틴 | `outstanding_infra_coros_` (엔진 전역) | 어댑터/프레임워크 |

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

// ✅ GOOD — per-core 복제 + co_await cross_core_post 전파
boost::unordered_flat_map<std::string, SessionId> local_channel_map_;  // per-core
// 변경 시 co_await cross_core_post()로 다른 코어에 전파 (§7)
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
    auto* other = ctx.local_registry.find<OtherService>();  // [D1]
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

// ✅ GOOD — CPU-bound 작업은 blocking_executor()로 offload (§7)
auto result = co_await blocking_executor().run([&] {
    return expensive_cpu_work();
});
```

### #6. 핸들러 error 반환의 의미

```cpp
// ⚠️ error() 반환 시 dispatch가 에러 프레임을 자동 전송함
// 대부분의 경우 직접 에러 응답을 보내고 ok()를 반환하는 게 올바른 패턴

// ✅ GOOD — 프레임워크 에러: ErrorCode만 사용
session->enqueue_write(ErrorSender::build_error_frame(msg_id, ErrorCode::InvalidMessage));
co_return ok();

// ✅ GOOD — 서비스별 에러: ServiceError + service_error_code 조합
session->enqueue_write(ErrorSender::build_error_frame(
    msg_id, ErrorCode::ServiceError, "",
    static_cast<uint16_t>(GatewayError::RouteNotFound)));
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
 ├── CoreEngine                       ← 스레드 풀, SPSC mesh 드레인, spawn_tracked
 │    └── PerCoreState[] (코어별 독립)
 │         ├── SessionManager         ← intrusive_ptr<Session> 소유, unique_ptr<SocketBase>
 │         ├── ServiceRegistry [D1]   ← 타입 기반 서비스 조회
 │         ├── MessageDispatcher      ← msg_id → handler 라우팅
 │         ├── BumpAllocator          ← 요청 스코프 할당
 │         ├── ArenaAllocator         ← 트랜잭션 스코프 할당
 │         ├── PeriodicTaskScheduler  ← 주기 태스크
 │         └── services[]             ← ServiceBaseInterface 인스턴스
 ├── Listener<P, T>[]                 ← 프로토콜별 (TCP, WebSocket 등), T::ListenerState 소유
 │    └── ConnectionHandler<P, T>[]   ← 코어별, handshake + read_loop + frame dispatch
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

### §10.2.1 CoreEngine::spawn_tracked()

어댑터/인프라 수준에서 코루틴을 실행할 때 사용. ServiceBase::spawn()과는 독립적인 추적 체계.

```cpp
// 어댑터 내부에서 — 예: KafkaAdapter 메시지 콜백
engine.spawn_tracked(0, [bridge, buf = std::move(pooled_buf)]()
    -> boost::asio::awaitable<void> {
    co_await bridge->dispatch(buf->span());
});
```

- `core_id`로 대상 io_context를 내부에서 결정 (io_context 직접 전달 금지)
- `outstanding_infra_coros_` atomic 카운터로 인프라 코루틴 수 추적
- 예외 발생 시 `spdlog::error`로 로깅 후 카운터 감소 (코루틴 누수 방지)
- **서비스 코드에서 직접 사용하지 않음** — 서비스는 `spawn()` 사용 (§7)

### §10.3 TCP 요청 처리 흐름

```
Client → TCP/TLS → TcpAcceptor → T::wrap_socket → unique_ptr<SocketBase>
                                     ↓
         ConnectionHandler<P,T>::accept_connection(SocketBase)
                  ↓
         Session(unique_ptr<SocketBase>) 생성
                  ↓
         read_loop: SocketBase::async_handshake() (TLS handshake / TCP no-op)
                  ↓
         SocketBase::async_read_some → Session(recv_buffer)
                  ↓
         P::try_decode(RingBuffer) → Frame
                  ↓
         MessageDispatcher::dispatch(msg_id, session, payload)
                  ↓
         Handler 코루틴 (route<T> / handle / default_handler)
                  ↓
         session->enqueue_write(response) → write_pump → SocketBase::async_write → Client
```

### §10.4 Kafka 메시지 흐름

```
[수신]
KafkaAdapter (consumer 스레드)
  → set_message_callback
  → [D6] consumer 메모리 풀에서 payload 복사
  → [D7] spawn_tracked() → KafkaDispatchBridge::dispatch(payload)
  → KafkaMessageMeta 파싱 → kafka_handler_map[msg_id] → Handler 코루틴

[응답 — Kafka-only 서비스]
Handler → send_response() → KafkaAdapter::produce(response_topic, envelope)

[응답 — Gateway 경유]
KafkaAdapter (consumer) → ResponseDispatcher → asio::post(session.core_id)
  → session->enqueue_write(response) → Client
```

### §10.5 ADR 포인터 테이블

| 주제 | 위치 | 설명 |
|------|------|------|
| io_context-per-core | `design-decisions.md` § 이벤트 루프 | shared-nothing 아키텍처 근거 |
| CRTP ServiceBase | `design-decisions.md` § 모듈 정체성 | 정적 다형성 선택 이유 |
| SPSC All-to-All Mesh 코어 간 통신 | `design-decisions.md` § 이벤트 루프 | 코어 쌍별 전용 SPSC 큐 (MPSC→SPSC 전환, v0.5.10.0) |
| 비동기 I/O 통합 | `design-decisions.md` § 비동기 I/O | Kafka/Redis/PG fd를 Asio에 등록 |
| shared-nothing 범위 | `design-decisions.md` § 성능 철학 | 외부 라이브러리 스레드 예외 |
| 코루틴 lifetime | `design-rationale.md` § 코루틴 수명 | intrusive_ptr 캡처, 수명 보장 |
| 메시지 디스패치 | `design-decisions.md` § 메시지 디스패치 | msg_id 기반 flat_map 라우팅 |
| 에러 핸들링 | `design-decisions.md` § 에러 핸들링 | std::expected<T, ErrorCode> |
| 설정 관리 | `design-decisions.md` § 설정 관리 | TOML + hot-reload |
| Protocol concept | `design-rationale.md` § Protocol concept | try_decode/consume_frame 의존성 역전 |
| FrameType concept | `protocol.hpp` | Frame 타입에 `payload()` accessor 요구. Protocol concept이 FrameType을 포함 |
| ServiceError sentinel | `error_code.hpp`, `error_response.fbs` | 서비스별 에러 코드 분리 — ErrorCode 범위 오염 방지 |

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
        const KafkaMessageMeta& meta, uint32_t msg_id, const LoginRequest* req) {
        auto email = req->email()->str();  // co_await 전 복사!
        auto pw = req->password()->str();

        auto pg_result = co_await pg_->query("SELECT ...", {email});
        // ... bcrypt 검증, JWT 생성, Redis 세션 저장 ...

        send_response(1001, meta.corr_id, meta.core_id,
                      meta.session_id, response_payload, {});
        co_return ok();
    }

private:
    // 에러 응답 헬퍼 — ServiceError sentinel + 서비스별 에러 코드
    void send_error(uint32_t msg_id, const KafkaMessageMeta& meta, AuthError err) {
        auto frame = ErrorSender::build_error_frame(
            msg_id, ErrorCode::ServiceError, "",
            static_cast<uint16_t>(err));
        send_response(msg_id, meta.corr_id, meta.core_id,
                      meta.session_id, frame, {});
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
