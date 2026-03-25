# 테스트 커버리지 보강 — CORE+SHARED 유닛 테스트 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** BACKLOG-148/191/192 — CORE+SHARED 유닛 테스트 22건 추가로 핵심 경로 커버리지 확보

**Architecture:** 기존 7개 테스트 파일에 케이스 추가 + 신규 3개 파일 생성. 소스 변경 2건(AdapterBase init 롤백, MetricsHttpServer local_port 접근자) 포함. GTest + Boost.Asio 코루틴 패턴.

**Tech Stack:** C++23, GTest, Boost.Asio, Boost.Beast (MetricsHttpServer)

**Spec:** `docs/test-coverage/plans/20260324_225121_test_coverage_core_shared_design.md`

---

## 의존성 그래프

```
Task 1 (소스 변경) ──┬──→ Task 2 (AdapterBase 테스트)
                     └──→ Task 3 (MetricsHttpServer 테스트)

Task 4~8: Task 1과 독립, 병렬 실행 가능
```

## 공통 규칙

- **저작권 헤더**: 새 파일 첫 줄에 `// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.`
- **테스트 패턴**: `apex_core/tests/test_helpers.hpp`의 `run_coro()`, `wait_for()`, `timeout_multiplier()` 활용
- **네이밍**: `TEST_F(ClassTest, ActionExpectedOutcome)`
- **CMake**: `apex_add_unit_test()` (core) 또는 `apex_shared_add_unit_test()` (shared) 매크로 사용
- **빌드**: 서브에이전트는 빌드 금지 — 메인이 전체 취합 후 직접 빌드
- **clang-format**: 코드 변경 후 서브에이전트가 해당 파일에 `clang-format -i` 실행

---

### Task 1: 소스 코드 사전 변경 (AdapterBase 롤백 + MetricsHttpServer local_port)

> Task 2, 3의 선행 조건. 반드시 먼저 완료.

**Files:**
- Modify: `apex_shared/lib/adapters/common/include/apex/shared/adapters/adapter_base.hpp` (init 메서드)
- Modify: `apex_core/include/apex/core/metrics_http_server.hpp` (local_port 추가)

- [ ] **Step 1: AdapterBase::init() — do_init() 예외 시 CLOSED 롤백 추가**

`adapter_base.hpp`의 `init()` 메서드에서 마지막 두 줄을 try-catch로 감싼다:

```cpp
// 변경 전 (라인 79-80):
state_.store(AdapterState::RUNNING, std::memory_order_release);
static_cast<Derived*>(this)->do_init(engine);

// 변경 후:
state_.store(AdapterState::RUNNING, std::memory_order_release);
try
{
    static_cast<Derived*>(this)->do_init(engine);
}
catch (...)
{
    state_.store(AdapterState::CLOSED, std::memory_order_release);
    base_logger_.error("do_init() threw — rolling back to CLOSED");
    throw;
}
```

- [ ] **Step 2: MetricsHttpServer — local_port() 접근자 추가**

`metrics_http_server.hpp`의 `stop()` 메서드 다음에 추가:

```cpp
/// 바인딩된 로컬 포트 반환 (port 0 사용 시 OS 할당 포트 확인용).
uint16_t local_port() const
{
    return acceptor_ ? acceptor_->local_endpoint().port() : 0;
}
```

- [ ] **Step 3: clang-format 적용**

```bash
clang-format -i apex_shared/lib/adapters/common/include/apex/shared/adapters/adapter_base.hpp
clang-format -i apex_core/include/apex/core/metrics_http_server.hpp
```

- [ ] **Step 4: 커밋**

```bash
git add apex_shared/lib/adapters/common/include/apex/shared/adapters/adapter_base.hpp \
        apex_core/include/apex/core/metrics_http_server.hpp
git commit -m "fix(core,shared): AdapterBase init 롤백 + MetricsHttpServer local_port 접근자 (BACKLOG-148,191)"
```

---

### Task 2: AdapterBaseFailureTest (신규 파일, 4건)

> Task 1 완료 후 실행. AdapterBase init 롤백이 적용된 상태에서 테스트.

**Files:**
- Create: `apex_shared/tests/unit/test_adapter_base_failure.cpp`
- Modify: `apex_shared/tests/unit/CMakeLists.txt`
- Reference: `apex_shared/tests/unit/test_adapter_base.cpp` (기존 mock 패턴 참고)
- Reference: `apex_shared/tests/test_mocks.hpp` (MockCoreEngine 등 확인)
- Reference: `apex_shared/lib/adapters/common/include/apex/shared/adapters/adapter_base.hpp` (CRTP 인터페이스)

- [ ] **Step 1: 기존 test_adapter_base.cpp 읽고 mock 패턴 파악**

`apex_shared/tests/unit/test_adapter_base.cpp`와 `apex_shared/tests/test_mocks.hpp`를 읽어서 MockCoreEngine, mock 어댑터 구성 방식을 파악한다.

- [ ] **Step 2: test_adapter_base_failure.cpp 작성**

FailingAdapter mock (CRTP) + AdapterBaseFailureTest 픽스처 + 4개 테스트 케이스:

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/adapter_base.hpp>
#include <gtest/gtest.h>
// + 기존 mock 패턴에 따라 CoreEngine mock 포함

namespace {

class FailingAdapter : public apex::shared::adapters::AdapterBase<FailingAdapter> {
    friend class apex::shared::adapters::AdapterBase<FailingAdapter>;
    bool should_fail_ = false;
    std::string_view do_name() const noexcept { return "FailingAdapter"; }
    void do_init(apex::core::CoreEngine& engine) {
        if (should_fail_) throw std::runtime_error("mock init failure");
    }
    void do_drain() {}
    void do_close() {}
    void do_close_per_core(uint32_t) {}
public:
    void set_fail(bool f) { should_fail_ = f; }
};

class AdapterBaseFailureTest : public ::testing::Test {
protected:
    // 기존 test_adapter_base.cpp의 MockCoreEngine 패턴을 따라 SetUp
};

// (1) do_init() 예외 → CLOSED 롤백
TEST_F(AdapterBaseFailureTest, InitFailureRollsBackToClosed) {
    adapter_.set_fail(true);
    EXPECT_THROW(adapter_.init(engine_), std::runtime_error);
    EXPECT_EQ(adapter_.state(), apex::core::AdapterState::CLOSED);
}

// (2) 이미 RUNNING인 상태에서 재init → 경고 + 무시
TEST_F(AdapterBaseFailureTest, DoubleInitWarns) {
    adapter_.set_fail(false);
    adapter_.init(engine_);
    EXPECT_EQ(adapter_.state(), apex::core::AdapterState::RUNNING);
    adapter_.init(engine_);  // 재호출 — 경고만, 상태 유지
    EXPECT_EQ(adapter_.state(), apex::core::AdapterState::RUNNING);
}

// (3) drain 5초 타임아웃 → 경고 로그 (close 완료는 보장)
TEST_F(AdapterBaseFailureTest, CloseTimeoutLogsWarning) {
    // close()의 타임아웃 경고 로직을 검증
    // adapter_base.hpp의 close() 내 5초 wait_for 로직 확인 후 구현
    // 핵심: SlowAdapter mock (do_drain에서 지연)으로 타임아웃 트리거
}

// (4) CLOSED(롤백) → 재init → RUNNING
TEST_F(AdapterBaseFailureTest, RecoveryAfterInitFailure) {
    adapter_.set_fail(true);
    EXPECT_THROW(adapter_.init(engine_), std::runtime_error);
    EXPECT_EQ(adapter_.state(), apex::core::AdapterState::CLOSED);
    adapter_.set_fail(false);
    adapter_.init(engine_);  // 복구
    EXPECT_EQ(adapter_.state(), apex::core::AdapterState::RUNNING);
}

} // namespace
```

**주의**: MockCoreEngine 구성은 기존 test_adapter_base.cpp 패턴을 정확히 따를 것. `core_count()`와 `io_context(i)` 제공이 핵심.

- [ ] **Step 3: CMakeLists.txt에 테스트 등록**

`apex_shared/tests/unit/CMakeLists.txt` 끝에 추가:

```cmake
apex_shared_add_unit_test(test_adapter_base_failure test_adapter_base_failure.cpp)
```

기존 test_adapter_base 등록 패턴을 참고하여 필요한 링크 타겟 추가.

- [ ] **Step 4: clang-format 적용**

```bash
clang-format -i apex_shared/tests/unit/test_adapter_base_failure.cpp
```

- [ ] **Step 5: 커밋**

```bash
git add apex_shared/tests/unit/test_adapter_base_failure.cpp apex_shared/tests/unit/CMakeLists.txt
git commit -m "test(shared): AdapterBase init 실패/복구 유닛 테스트 추가 (BACKLOG-148,192)"
```

---

### Task 3: MetricsHttpServerTest (신규 파일, 6건)

> Task 1 완료 후 실행. local_port() 접근자가 적용된 상태에서 테스트.

**Files:**
- Create: `apex_core/tests/unit/test_metrics_http_server.cpp`
- Modify: `apex_core/tests/unit/CMakeLists.txt`
- Reference: `apex_core/src/metrics_http_server.cpp` (handle_session 로직)
- Reference: `apex_core/include/apex/core/metrics_http_server.hpp`
- Reference: `apex_core/include/apex/core/metrics_registry.hpp` (MetricsRegistry 인터페이스)

- [ ] **Step 1: MetricsRegistry 인터페이스 파악**

`apex_core/include/apex/core/metrics_registry.hpp`를 읽어서 생성자, `serialize()` 반환값 형식 확인.

- [ ] **Step 2: test_metrics_http_server.cpp 작성**

port 0 바인딩 + raw TCP 요청 패턴. Boost.Beast는 서버에서만 사용하고, 테스트 클라이언트는 raw socket.

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/metrics_http_server.hpp>
#include <apex/core/metrics_registry.hpp>
#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <thread>

namespace {

using boost::asio::ip::tcp;

class MetricsHttpServerTest : public ::testing::Test {
protected:
    boost::asio::io_context io_ctx_;
    apex::core::MetricsRegistry registry_;
    std::atomic<bool> running_{true};
    apex::core::MetricsHttpServer server_;
    uint16_t port_{0};
    std::jthread server_thread_;

    void SetUp() override {
        server_.start(io_ctx_, 0, registry_, running_);
        port_ = server_.local_port();
        ASSERT_NE(port_, 0);
        server_thread_ = std::jthread([this] { io_ctx_.run(); });
    }

    void TearDown() override {
        server_.stop();
        io_ctx_.stop();
        // server_thread_ 자동 join (jthread)
    }

    // raw TCP HTTP 요청 헬퍼
    std::string http_request(const std::string& method, const std::string& path) {
        boost::asio::io_context client_io;
        tcp::socket sock(client_io);
        sock.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port_));
        std::string req = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        boost::asio::write(sock, boost::asio::buffer(req));
        std::string response;
        boost::system::error_code ec;
        boost::asio::read(sock, boost::asio::dynamic_buffer(response), ec);
        return response;
    }

    int extract_status_code(const std::string& response) {
        // "HTTP/1.1 200 OK" → 200
        auto pos = response.find(' ');
        if (pos == std::string::npos) return 0;
        return std::stoi(response.substr(pos + 1, 3));
    }
};

TEST_F(MetricsHttpServerTest, MetricsEndpointReturnsPrometheusFormat) {
    auto resp = http_request("GET", "/metrics");
    EXPECT_EQ(extract_status_code(resp), 200);
    EXPECT_NE(resp.find("text/plain"), std::string::npos);
}

TEST_F(MetricsHttpServerTest, HealthEndpointReturnsOk) {
    auto resp = http_request("GET", "/health");
    EXPECT_EQ(extract_status_code(resp), 200);
    EXPECT_NE(resp.find("OK"), std::string::npos);
}

TEST_F(MetricsHttpServerTest, ReadyEndpointReflectsRunningFlag) {
    running_.store(true);
    auto resp1 = http_request("GET", "/ready");
    EXPECT_EQ(extract_status_code(resp1), 200);

    running_.store(false);
    auto resp2 = http_request("GET", "/ready");
    EXPECT_EQ(extract_status_code(resp2), 503);
}

TEST_F(MetricsHttpServerTest, UnknownPathReturns404) {
    auto resp = http_request("GET", "/invalid");
    EXPECT_EQ(extract_status_code(resp), 404);
}

TEST_F(MetricsHttpServerTest, UnsupportedMethodReturns404) {
    auto resp = http_request("POST", "/metrics");
    EXPECT_EQ(extract_status_code(resp), 404);
}

TEST_F(MetricsHttpServerTest, ConcurrentConnections) {
    constexpr int N = 5;
    std::vector<std::jthread> threads;
    std::atomic<int> success_count{0};
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&] {
            auto resp = http_request("GET", "/health");
            if (extract_status_code(resp) == 200) success_count.fetch_add(1);
        });
    }
    threads.clear();  // join all
    EXPECT_EQ(success_count.load(), N);
}

} // namespace
```

**주의**: MetricsHttpServer::start()의 실제 시그니처와 MetricsRegistry 생성자를 확인하여 픽스처를 조정할 것. server_thread_ io_ctx_.run()이 블로킹되므로 TearDown에서 stop+io_ctx_.stop 순서 중요.

- [ ] **Step 3: CMakeLists.txt에 테스트 등록**

`apex_core/tests/unit/CMakeLists.txt`에 추가. Boost::beast 링크 필요 여부 확인 (서버가 이미 beast를 사용하므로 apex_core 타겟에 포함되어 있을 것):

```cmake
apex_add_unit_test(test_metrics_http_server test_metrics_http_server.cpp)
# 필요 시 target_link_libraries에 Boost::beast 추가
```

- [ ] **Step 4: clang-format + 커밋**

```bash
clang-format -i apex_core/tests/unit/test_metrics_http_server.cpp
git add apex_core/tests/unit/test_metrics_http_server.cpp apex_core/tests/unit/CMakeLists.txt
git commit -m "test(core): MetricsHttpServer 유닛 테스트 추가 — 6개 엔드포인트/동시성 (BACKLOG-191)"
```

---

### Task 4: RedisMultiplexer close 안전성 테스트 (신규 파일, 2건)

> Task 1과 독립. 병렬 실행 가능.

**Files:**
- Create: `apex_shared/tests/unit/test_redis_multiplexer_close.cpp`
- Modify: `apex_shared/tests/unit/CMakeLists.txt`
- Reference: `apex_shared/tests/unit/test_redis_multiplexer.cpp` (기존 패턴)
- Reference: `apex_shared/lib/adapters/redis/include/apex/shared/adapters/redis/redis_multiplexer.hpp`

- [ ] **Step 1: 기존 test_redis_multiplexer.cpp 패턴 파악**

기존 테스트에서 RedisMultiplexer 생성 방식 (io_context, config, core_id, spawn_cb) 확인.

- [ ] **Step 2: test_redis_multiplexer_close.cpp 작성**

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

// 기존 test_redis_multiplexer.cpp의 include/setup 패턴을 따름

namespace {

class RedisMultiplexerCloseTest : public ::testing::Test {
protected:
    // 기존 테스트의 io_context + config + multiplexer 생성 패턴 재사용
};

// (1) pending command 있을 때 close() → 콜백이 에러로 완료
TEST_F(RedisMultiplexerCloseTest, CloseWithPendingCommandsReturnsError) {
    // command() 호출 후 (응답 없이) 즉시 close()
    // pending command의 콜백이 AdapterError로 resolve되는지 검증
}

// (2) close() 후 command() → 즉시 에러
TEST_F(RedisMultiplexerCloseTest, CommandAfterCloseReturnsError) {
    // close() 호출 후 command() → 에러 반환
    // use-after-free 없음 확인 (ASAN에서 검증)
}

} // namespace
```

**핵심**: RedisMultiplexer는 실제 Redis 연결 없이 pending 상태를 만들 수 있는지 확인. 연결 없이 command()를 호출하면 어떤 상태가 되는지 파악 필수. `connected()` false 상태에서 command() → reconnect 시도 or 즉시 에러. close() 후 상태 검증.

- [ ] **Step 3: CMakeLists.txt + clang-format + 커밋**

```bash
clang-format -i apex_shared/tests/unit/test_redis_multiplexer_close.cpp
git add apex_shared/tests/unit/test_redis_multiplexer_close.cpp apex_shared/tests/unit/CMakeLists.txt
git commit -m "test(shared): RedisMultiplexer close 안전성 테스트 추가 (BACKLOG-148)"
```

---

### Task 5: CircuitBreaker 코루틴 동시성 테스트 (기존 확장, 2건)

> Task 1과 독립. 병렬 실행 가능.

**Files:**
- Modify: `apex_shared/tests/unit/test_circuit_breaker.cpp`
- Reference: `apex_shared/lib/adapters/common/include/apex/shared/adapters/circuit_breaker.hpp`

- [ ] **Step 1: 기존 test_circuit_breaker.cpp 끝에 ConcurrencyTest 픽스처 추가**

CircuitBreaker 상태 변수가 비atomic이므로 **단일 io_context + 다중 co_spawn** 패턴:

```cpp
class CircuitBreakerConcurrencyTest : public ::testing::Test {
protected:
    boost::asio::io_context io_ctx_;
    // CircuitBreakerConfig는 기존 테스트의 config 패턴 참고
};

// (1) 4개 코루틴 동시 실패 → failure_count 정합성
TEST_F(CircuitBreakerConcurrencyTest, ConcurrentCoroCallsCountConsistent) {
    CircuitBreakerConfig config{.failure_threshold = 10, .open_duration = std::chrono::seconds{5}, .half_open_max_calls = 2};
    CircuitBreaker breaker(config, logger);
    constexpr int N = 4;
    std::atomic<int> completed{0};

    for (int i = 0; i < N; ++i) {
        boost::asio::co_spawn(io_ctx_, [&]() -> boost::asio::awaitable<void> {
            auto result = breaker.call([&]() -> boost::asio::awaitable<Result<int>> {
                co_return std::unexpected(ErrorCode::ConnectionFailed);
            });
            co_await result;  // call()이 awaitable 반환
            completed.fetch_add(1);
        }, boost::asio::detached);
    }

    io_ctx_.run();
    EXPECT_EQ(completed.load(), N);
    EXPECT_EQ(breaker.failure_count(), N);
}

// (2) HALF_OPEN에서 동시 call() → max_calls 초과 시 CircuitOpen
TEST_F(CircuitBreakerConcurrencyTest, HalfOpenThrottlingUnderConcurrency) {
    // breaker를 OPEN으로 전이 → open_duration 경과 → HALF_OPEN
    // half_open_max_calls=2 설정, 4개 코루틴 co_spawn
    // 2개는 성공적으로 call() 진입, 나머지는 CircuitOpen 반환 검증
}
```

**주의**: CircuitBreaker 생성자 시그니처, logger 파라미터, ErrorCode 네임스페이스를 기존 테스트에서 정확히 확인할 것. `call()`의 반환 타입이 `std::invoke_result_t<F>`이므로 코루틴 callable의 반환 타입에 맞춰 작성.

- [ ] **Step 2: clang-format + 커밋**

```bash
clang-format -i apex_shared/tests/unit/test_circuit_breaker.cpp
git add apex_shared/tests/unit/test_circuit_breaker.cpp
git commit -m "test(shared): CircuitBreaker 코루틴 동시성 테스트 추가 (BACKLOG-148)"
```

---

### Task 6: Session 동시성 테스트 (기존 확장, 2건)

> Task 1과 독립. 병렬 실행 가능.

**Files:**
- Modify: `apex_core/tests/unit/test_session.cpp`
- Reference: `apex_core/include/apex/core/session.hpp`
- Reference: `apex_core/tests/test_helpers.hpp` (make_socket_pair, run_coro)

- [ ] **Step 1: 기존 test_session.cpp 끝에 ConcurrencyTest 픽스처 추가**

```cpp
class SessionConcurrencyTest : public ::testing::Test {
protected:
    boost::asio::io_context io_ctx_;
    // make_socket_pair()로 소켓 생성, Session 생성
    // 기존 SessionTest 픽스처 패턴 참고
};

// (1) 2개 코루틴 동시 async_send → 모두 도착
TEST_F(SessionConcurrencyTest, ConcurrentAsyncSendAllDelivered) {
    // co_spawn 2개로 동시에 async_send() 호출
    // write_queue 또는 소켓에서 2개 프레임 모두 수신 확인
}

// (2) max_queue_depth 초과 → BufferFull
TEST_F(SessionConcurrencyTest, EnqueueWriteMaxDepthReturnsBufferFull) {
    // Session 생성 시 max_queue_depth=4 (작게 설정)
    // 5번째 enqueue_write() → BufferFull 에러
}
```

**주의**: Session 생성자 시그니처(SessionId, socket, core_id, recv_buf_capacity, max_queue_depth) 확인. max_queue_depth 기본값은 256이므로 테스트용으로 작은 값 설정.

- [ ] **Step 2: clang-format + 커밋**

```bash
clang-format -i apex_core/tests/unit/test_session.cpp
git add apex_core/tests/unit/test_session.cpp
git commit -m "test(core): Session 동시성 테스트 추가 — async_send + BufferFull (BACKLOG-148)"
```

---

### Task 7: 단순 에러/엣지케이스 — TimingWheel + FrameCodec + Config (기존 확장, 4건)

> Task 1과 독립. 병렬 실행 가능.

**Files:**
- Modify: `apex_core/tests/unit/test_timing_wheel.cpp`
- Modify: `apex_core/tests/unit/test_frame_codec.cpp`
- Modify: `apex_core/tests/unit/test_config.cpp`
- Reference: `apex_core/include/apex/core/timing_wheel.hpp` (schedule, num_slots)
- Reference: `apex_core/include/apex/core/frame_codec.hpp` + `apex_core/src/frame_codec.cpp` (ErrorCode)
- Reference: `apex_core/src/config.cpp` (num_cores 파싱)

- [ ] **Step 1: test_timing_wheel.cpp에 2건 추가**

기존 테스트 파일 끝에 추가:

```cpp
// num_slots=1 → power-of-2 처리 검증
TEST_F(TimingWheelTest, SingleSlotCreation) {
    size_t expired = 0;
    TimingWheel wheel(1, [&](auto) { ++expired; });
    auto id = wheel.schedule(1);
    wheel.tick();
    EXPECT_EQ(expired, 1);
}

// 슬롯 1개에 다중 엔트리 → tick() 시 전부 만료
TEST_F(TimingWheelTest, SingleSlotMultipleEntries) {
    std::vector<TimingWheel::EntryId> ids;
    size_t expired = 0;
    TimingWheel wheel(1, [&](auto) { ++expired; });
    for (int i = 0; i < 5; ++i)
        ids.push_back(wheel.schedule(1));
    wheel.tick();
    EXPECT_EQ(expired, 5);
}
```

**주의**: TimingWheel 생성자가 (num_slots, callback) 형태인지 확인. `schedule(ticks_from_now)`에서 ticks_from_now=0이면 즉시 만료인지, 다음 tick인지 확인. `num_slots=1`일 때 `schedule(1)`이 out_of_range를 던지는지도 확인 — 만약 그렇다면 `schedule(0)` 또는 다른 값으로 조정.

- [ ] **Step 2: test_frame_codec.cpp에 1건 추가**

```cpp
// 미지원 프로토콜 버전 → UnsupportedProtocolVersion 에러
TEST_F(FrameCodecTest, UnsupportedProtocolVersionReturnsError) {
    // WireHeader를 수동 구성하되 version 필드를 CURRENT_VERSION + 1로 설정
    // 버퍼에 encode 후 try_decode() → ErrorCode::UnsupportedProtocolVersion
    // build_frame() 헬퍼를 참고하되 version 필드를 변조
}
```

**주의**: `WireHeader::CURRENT_VERSION` 상수 확인. try_decode()에서 버전 체크가 어떤 시점에 이루어지는지 frame_codec.cpp를 읽고 정확한 바이트 위치에 변조된 버전을 삽입.

- [ ] **Step 3: test_config.cpp에 1건 추가**

```cpp
// num_cores=0 → Config 로드 성공, 값 0 저장
TEST_F(ConfigTest, NumCoresZeroPassesConfigLoad) {
    auto path = write_toml("zero_cores.toml", R"(
[server]
num_cores = 0
)");
    auto cfg = apex::core::AppConfig::from_file(path);
    EXPECT_EQ(cfg.server.num_cores, 0);
}
```

**주의**: `write_toml()` 헬퍼는 기존 ConfigTest 픽스처에 이미 있음. `cfg.server.num_cores` 필드명을 ServerConfig 구조체에서 확인.

- [ ] **Step 4: clang-format + 커밋**

```bash
clang-format -i apex_core/tests/unit/test_timing_wheel.cpp \
              apex_core/tests/unit/test_frame_codec.cpp \
              apex_core/tests/unit/test_config.cpp
git add apex_core/tests/unit/test_timing_wheel.cpp \
        apex_core/tests/unit/test_frame_codec.cpp \
        apex_core/tests/unit/test_config.cpp
git commit -m "test(core): TimingWheel/FrameCodec/Config 에러 경로 테스트 추가 (BACKLOG-148)"
```

---

### Task 8: CrossCoreCall 타임아웃 + SpscMesh Death Test (기존 확장, 2건)

> Task 1과 독립. 병렬 실행 가능.

**Files:**
- Modify: `apex_core/tests/unit/test_cross_core_call.cpp`
- Modify: `apex_core/tests/unit/test_spsc_mesh.cpp`
- Reference: `apex_core/include/apex/core/cross_core_call.hpp`
- Reference: `apex_core/include/apex/core/spsc_mesh.hpp`

- [ ] **Step 1: test_cross_core_call.cpp에 동일 코어 호출 테스트 추가**

기존 테스트의 Server 생성/스레드 실행 패턴을 따라 추가:

```cpp
// 동일 코어 호출 → 데드락 → 짧은 타임아웃으로 CrossCoreTimeout 반환
TEST_F(CrossCoreCallTest, SameCoreCallReturnsTimeout) {
    // cross_core_call(engine, current_core_id, fn, 100ms) 호출
    // 결과: CrossCoreTimeout 에러
    // 기존 테스트의 server + thread 패턴 사용
    // 타임아웃을 100ms로 짧게 설정하여 테스트 시간 최소화
}
```

**주의**: `cross_core_call()`의 timeout 파라미터 위치 확인 (4번째 인자, 기본 5초). 기존 테스트에서 Server 객체의 engine 접근 방법 확인.

- [ ] **Step 2: test_spsc_mesh.cpp에 DeathTest 추가**

```cpp
class SpscMeshDeathTest : public ::testing::Test {
protected:
    void SetUp() override {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
    }
};

TEST_F(SpscMeshDeathTest, SelfCoreQueueAsserts) {
#ifdef NDEBUG
    GTEST_SKIP() << "assert disabled in Release";
#else
    // SpscMesh(num_cores, queue_capacity, core_io_contexts)
    boost::asio::io_context io_ctxs_raw[2];
    std::vector<boost::asio::io_context*> ptrs = {&io_ctxs_raw[0], &io_ctxs_raw[1]};
    apex::core::SpscMesh mesh(2, 64, ptrs);
    EXPECT_DEATH(mesh.queue(0, 0), "");
#endif
}
```

**주의**: SpscMesh 생성자의 3번째 파라미터 타입 확인 (`std::vector<io_context*>` vs `std::span` 등). GTest DeathTest 네이밍 규칙: 클래스명에 `DeathTest` 포함 시 GTest가 자동으로 fork 모드 적용.

- [ ] **Step 3: clang-format + 커밋**

```bash
clang-format -i apex_core/tests/unit/test_cross_core_call.cpp \
              apex_core/tests/unit/test_spsc_mesh.cpp
git add apex_core/tests/unit/test_cross_core_call.cpp \
        apex_core/tests/unit/test_spsc_mesh.cpp
git commit -m "test(core): CrossCoreCall 동일코어 타임아웃 + SpscMesh death test (BACKLOG-148)"
```
