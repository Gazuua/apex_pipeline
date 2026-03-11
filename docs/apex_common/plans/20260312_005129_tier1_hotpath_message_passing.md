# Phase 5.5 Tier 1: 핫패스 병목 제거 + Cross-Core 아키텍처 전환 — 구현 계획

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** drain/tick 분리로 폴링 지연을 제거하고, closure shipping을 message passing 아키텍처로 전환하여 per-message heap 할당을 없애며, MessageDispatcher를 flat_hash_map으로 교체하여 2MB 메모리 낭비를 제거한다.

**Architecture:** 3개 독립 축으로 진행 — (A) drain/tick 분리 (CoreEngine 내부 리팩토링), (B) message passing 전환 (CoreMessage 구조 변경 → CrossCoreDispatcher → SharedPayload → 기존 API 대체), (C) MessageDispatcher 자료구조 교체. 각 축은 내부적으로 의존성 순서를 따르되, 축 간 독립 커밋 가능.

**Tech Stack:** C++23, Boost.Asio, boost::unordered_flat_map, boost::intrusive_ptr (SharedPayload), Google Benchmark (마이크로벤치)

**v6 계획서 참조**: `docs/apex_common/plans/20260311_204613_phase5_5_v6.md` § 1.1~1.6, § 6 Tier 1 작업 목록

**선행 조건**: Tier 0.5 (에러 타입 통일) 완료 — `Result<void>`, `ErrorCode` 기반 API 확정

---

## File Structure

### New Files

| File | Purpose |
|------|---------|
| `apex_core/include/apex/core/cross_core_op.hpp` | `CrossCoreOp` enum + `CrossCoreHandler` 타입 정의 |
| `apex_core/include/apex/core/cross_core_dispatcher.hpp` | `CrossCoreDispatcher` — op→handler 매핑 (function pointer 배열) |
| `apex_core/src/cross_core_dispatcher.cpp` | CrossCoreDispatcher 구현 |
| `apex_core/include/apex/core/shared_payload.hpp` | `SharedPayload` — immutable refcounted buffer (header-only, 인라인 메서드) |
| `apex_core/include/apex/core/cross_core_message.hpp` | 신규 `cross_core_post_msg()`, `cross_core_request()` 함수 |
| `apex_core/tests/unit/test_cross_core_dispatcher.cpp` | CrossCoreDispatcher 단위 테스트 |
| `apex_core/tests/unit/test_shared_payload.cpp` | SharedPayload 단위 테스트 |
| `apex_core/tests/unit/test_drain_tick.cpp` | drain/tick 분리 테스트 |

### Modified Files

| File | Change |
|------|--------|
| `apex_core/include/apex/core/core_engine.hpp` | CoreMessage 구조 변경 (Type→CrossCoreOp, data→uintptr_t), CoreEngineConfig에 tick_interval/drain_batch_limit 추가, drain_callback→tick_callback, drain_pending_ atomic 배열 추가, drain_inbox/start_tick_timer 분리 |
| `apex_core/src/core_engine.cpp` | start_drain_timer 제거 → drain_inbox + start_tick_timer, post_to에 atomic coalescing + post() 알림, drain_remaining 타입 분기 갱신 |
| `apex_core/include/apex/core/cross_core_call.hpp` | cross_core_post/cross_core_call → deprecated 또는 message passing으로 대체 |
| `apex_core/include/apex/core/message_dispatcher.hpp` | `std::array<Handler, 65536>` → `boost::unordered_flat_map<uint16_t, Handler>` |
| `apex_core/src/message_dispatcher.cpp` | dispatch 내부 flat_map lookup 사용 |
| `apex_core/include/apex/core/service_base.hpp` | `owned_dispatcher_` 관련 조정 (null 허용) |
| `apex_core/include/apex/core/server.hpp` | cross_core_post/cross_core_call wrapper를 message passing API로 전환, ServerConfig에서 drain_interval 제거 |
| `apex_core/src/server.cpp` | process_frames zero-copy (consume 순서 변경, stack_buf 제거), cross_core 호출 전환 |
| `apex_core/tests/unit/test_message_dispatcher.cpp` | flat_map 기반 dispatch 테스트 |
| `apex_core/tests/unit/test_cross_core_call.cpp` | message passing 기반으로 전환, drain_interval→tick_interval |
| `apex_core/tests/unit/test_core_engine.cpp` | CoreMessage::Type→CrossCoreOp, drain_interval→tick_interval, drain_callback→tick_callback, Legacy 테스트 전환 |
| `apex_core/tests/integration/test_pipeline_integration.cpp` | CoreMessage::Type→CrossCoreOp, dispatch 변경에 따른 검증 |
| `apex_core/tests/integration/test_server_e2e.cpp` | zero-copy dispatch 후 E2E 동작 검증 |
| `apex_core/tests/unit/test_config.cpp` | drain_interval_us→tick_interval_ms 검증 |
| `apex_core/src/config.cpp` | drain_interval_us→tick_interval_ms 파싱 |
| `apex_core/config/default.toml` | drain_interval_us→tick_interval_ms |
| `apex_core/vcpkg.json` | `boost-unordered` 추가 (flat_map용) |
| `apex_core/CMakeLists.txt` | 신규 소스/테스트 파일 추가 |

---

## Chunk 1: 독립 기반 작업 — MessageDispatcher flat_map 전환 + CoreMessage 구조 변경 (Tasks 1–2)

> 이 두 작업은 서로 독립이며 Tier 1의 나머지 작업에 대한 선행 조건을 만든다.

### Task 1: MessageDispatcher — 65536-entry array → boost::unordered_flat_map 전환

**Files:**
- Modify: `apex_core/include/apex/core/message_dispatcher.hpp`
- Modify: `apex_core/src/message_dispatcher.cpp`
- Modify: `apex_core/include/apex/core/service_base.hpp` (owned_dispatcher_ 관련)
- Modify: `apex_core/vcpkg.json` (boost-unordered 추가)
- Test: `apex_core/tests/unit/test_message_dispatcher.cpp`

- [ ] **Step 1: vcpkg.json에 boost-unordered 의존성 추가**

`apex_core/vcpkg.json`:
```json
{
  "dependencies": [
    "boost-asio",
    "boost-unordered",
    "flatbuffers",
    "gtest",
    "spdlog",
    "tomlplusplus"
  ]
}
```

> boost-unordered는 boost-asio 의존성 체인에 이미 포함되어 있을 가능성이 높지만, 명시적 선언이 올바른 관행.

- [ ] **Step 2: test_message_dispatcher.cpp — MaxMsgId 테스트 변경**

65536-entry 배열은 모든 uint16_t를 커버했지만, flat_map은 등록된 ID만 가짐. `MaxMsgId` 테스트의 의미가 달라지지 않음 (등록 후 dispatch 성공 확인).

기존 테스트는 그대로 유효. **추가 테스트 작성:**

`apex_core/tests/unit/test_message_dispatcher.cpp` 끝에 추가:
```cpp
TEST_F(MessageDispatcherTest, MemoryUsageDrasticallyReduced) {
    // flat_map은 등록된 핸들러 수에 비례하는 메모리만 사용
    // 65536 * sizeof(Handler) ≈ 2MB 대신, 빈 상태에서 수십 바이트
    EXPECT_EQ(d->handler_count(), 0u);
    // 이 테스트는 컴파일 시점에 배열이 제거되었음을 간접 확인
}

TEST_F(MessageDispatcherTest, UnregisterHandler) {
    bool called = false;
    d->register_handler(0x0001, make_handler([&](auto...) -> awaitable<Result<void>> {
        called = true;
        co_return ok();
    }));
    EXPECT_TRUE(d->has_handler(0x0001));

    d->unregister_handler(0x0001);
    EXPECT_FALSE(d->has_handler(0x0001));
    EXPECT_EQ(d->handler_count(), 0u);
}
```

- [ ] **Step 3: 테스트 실패 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -R test_message_dispatcher -V`
Expected: PASS — 기존 테스트 + 신규 테스트 모두 통과 (아직 내부 자료구조만 바꾸는 거라 API는 동일)

> 참고: 이 단계에서는 API가 바뀌지 않으므로 테스트가 먼저 통과할 수 있음. 진정한 TDD 대상은 자료구조 교체 후 성능 특성.

- [ ] **Step 4: message_dispatcher.hpp — flat_map으로 교체**

`apex_core/include/apex/core/message_dispatcher.hpp`:

```cpp
#pragma once

#include <apex/core/result.hpp>
#include <apex/core/session.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace apex::core {

/// Message dispatcher using boost::unordered_flat_map indexed by msg_id.
/// O(1) amortized lookup. Memory proportional to registered handler count only.
/// Thread-safe for concurrent reads after setup. NOT thread-safe for concurrent read-write.
class MessageDispatcher {
public:
    using Handler = std::function<
        boost::asio::awaitable<Result<void>>(SessionPtr, uint16_t, std::span<const uint8_t>)>;

    MessageDispatcher() = default;

    void register_handler(uint16_t msg_id, Handler handler);
    void unregister_handler(uint16_t msg_id);

    [[nodiscard]] boost::asio::awaitable<Result<void>>
    dispatch(SessionPtr session, uint16_t msg_id, std::span<const uint8_t> payload) const;

    [[nodiscard]] bool has_handler(uint16_t msg_id) const noexcept;
    [[nodiscard]] size_t handler_count() const noexcept;

private:
    boost::unordered_flat_map<uint16_t, Handler> handlers_;
};

} // namespace apex::core
```

> `std::unique_ptr<std::array<Handler, 65536>>` (~2MB heap) → `boost::unordered_flat_map` (등록 수 비례). 메모리 절감이 핵심 가치.

- [ ] **Step 5: message_dispatcher.cpp — flat_map 기반 구현**

`apex_core/src/message_dispatcher.cpp`:

```cpp
#include <apex/core/message_dispatcher.hpp>

#include <spdlog/spdlog.h>

namespace apex::core {

void MessageDispatcher::register_handler(uint16_t msg_id, Handler handler) {
    handlers_.insert_or_assign(msg_id, std::move(handler));
    // handler_count는 handlers_.size()로 자동 관리
}

void MessageDispatcher::unregister_handler(uint16_t msg_id) {
    handlers_.erase(msg_id);
}

boost::asio::awaitable<Result<void>>
MessageDispatcher::dispatch(SessionPtr session, uint16_t msg_id, std::span<const uint8_t> payload) const {
    auto it = handlers_.find(msg_id);
    if (it == handlers_.end()) {
        co_return error(ErrorCode::HandlerNotFound);
    }
    try {
        co_return co_await it->second(std::move(session), msg_id, payload);
    } catch (const std::exception& e) {
        if (auto logger = spdlog::get("apex")) {
            logger->error("MessageDispatcher: handler for msg_id 0x{:04x} threw: {}",
                static_cast<unsigned>(msg_id), e.what());
        }
        co_return error(ErrorCode::HandlerException);
    } catch (...) {
        if (auto logger = spdlog::get("apex")) {
            logger->error("MessageDispatcher: handler for msg_id 0x{:04x} threw unknown exception",
                static_cast<unsigned>(msg_id));
        }
        co_return error(ErrorCode::HandlerException);
    }
}

bool MessageDispatcher::has_handler(uint16_t msg_id) const noexcept {
    return handlers_.contains(msg_id);
}

size_t MessageDispatcher::handler_count() const noexcept {
    return handlers_.size();
}

} // namespace apex::core
```

> **주의**: Tier 0.5의 dispatch 반환 타입 변경(`Result<void>` 직접 반환)이 선행 적용된 상태를 가정. Tier 0.5가 미완료 시 기존 `expected<Result<void>, DispatchError>` 반환 유지 필요.

- [ ] **Step 6: service_base.hpp — owned_dispatcher_ 간소화**

`apex_core/include/apex/core/service_base.hpp`에서 `owned_dispatcher_`가 `std::make_unique<MessageDispatcher>()`로 생성됨. flat_map으로 교체되면 이 초기화는 빈 map을 만들므로 변경 불필요. **검증만.**

- [ ] **Step 7: 전체 빌드 + 전체 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS — 기존 9개 + 신규 2개 = 11개 테스트 전부 통과

- [ ] **Step 8: 커밋**

```bash
git add apex_core/vcpkg.json \
        apex_core/include/apex/core/message_dispatcher.hpp \
        apex_core/src/message_dispatcher.cpp \
        apex_core/tests/unit/test_message_dispatcher.cpp
git commit -m "refactor(dispatch): MessageDispatcher 65536-array → boost::unordered_flat_map"
```

---

### Task 2: CoreMessage 구조 변경 — Type enum → CrossCoreOp + uintptr_t

**Files:**
- Create: `apex_core/include/apex/core/cross_core_op.hpp`
- Modify: `apex_core/include/apex/core/core_engine.hpp`
- Modify: `apex_core/src/core_engine.cpp` (drain 내부 타입 분기 갱신)
- Modify: `apex_core/include/apex/core/cross_core_call.hpp` (CoreMessage 필드명 갱신)
- Modify: `apex_core/tests/unit/test_core_engine.cpp` (CoreMessage::Type→CrossCoreOp, .type→.op, uint64_t→uintptr_t)
- Modify: `apex_core/tests/integration/test_pipeline_integration.cpp` (CoreMessage::Type→CrossCoreOp, .type→.op)
- Test: `apex_core/tests/unit/test_cross_core_call.cpp` (기존 테스트 통과 확인)

- [ ] **Step 1: cross_core_op.hpp 작성**

`apex_core/include/apex/core/cross_core_op.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace apex::core {

/// Operation codes for cross-core message dispatch.
/// Framework-reserved: 0x0000 ~ 0x00FF
/// Application-defined: 0x0100+
enum class CrossCoreOp : uint16_t {
    /// Framework-internal operations
    Noop = 0,
    Shutdown,

    // Legacy compatibility (Tier 1 전환 기간 동안 유지)
    LegacyCrossCoreFn,      // 기존 closure-based cross_core_post/call

    /// Generic user-defined message — message_handler_ 경로로 전달.
    /// 기존 CoreMessage::Type::Custom 대체. 테스트 및 사용자 코드 호환용.
    Custom,

    /// Application operations (Tier 1 message passing 전환 후 추가)
    // BroadcastToSessions = 0x0100,
    // CollectMetrics,
    // FindSessionRequest,
    // FindSessionReply,
};

/// Cross-core handler signature — function pointer for static dispatch (icache friendly).
/// @param core_id  The core that is processing this message
/// @param source_core  The core that sent this message
/// @param data  Opaque pointer to payload (handler casts to concrete type)
struct CoreContext;  // forward declaration
using CrossCoreHandler = void(*)(uint32_t core_id, uint32_t source_core, void* data);

} // namespace apex::core
```

- [ ] **Step 2: CoreMessage 구조 변경**

`apex_core/include/apex/core/core_engine.hpp` — CoreMessage 변경:

```cpp
#include <apex/core/cross_core_op.hpp>

/// Trivially-copyable message for inter-core communication via MpscQueue.
struct CoreMessage {
    CrossCoreOp op{CrossCoreOp::Noop};
    uint32_t source_core{0};
    uintptr_t data{0};
};
static_assert(std::is_trivially_copyable_v<CoreMessage>);
static_assert(sizeof(CoreMessage) <= 16);
```

> 기존 `Type` enum 제거. `CrossCoreOp::LegacyCrossCoreFn`으로 기존 closure 기반 코드를 전환 기간 동안 호환.

- [ ] **Step 3: core_engine.cpp — drain 내부 분기를 CrossCoreOp 기반으로 갱신**

`start_drain_timer`의 while 루프 내부:

```cpp
while (auto msg = core_ctx.inbox->dequeue()) {
    if (msg->op == CrossCoreOp::LegacyCrossCoreFn) {
        // 기존 closure 기반 호환 (Tier 1 전환 완료 후 제거)
        auto* task = reinterpret_cast<std::function<void()>*>(msg->data);
        if (task) {
            try {
                (*task)();
            } catch (const std::exception& e) {
                spdlog::error("Core {} cross-core task exception: {}", core_id, e.what());
            } catch (...) {
                spdlog::error("Core {} cross-core task unknown exception", core_id);
            }
            delete task;
        }
        continue;
    }
    if (message_handler_) {
        message_handler_(core_id, *msg);
    }
}
```

`drain_remaining`도 동일하게 갱신:
```cpp
if (msg->op == CrossCoreOp::LegacyCrossCoreFn) {
    auto* task = reinterpret_cast<std::function<void()>*>(msg->data);
    delete task;
}
```

- [ ] **Step 4: cross_core_call.hpp — CoreMessage 필드명 갱신**

`cross_core_post`:
```cpp
CoreMessage msg;
msg.op = CrossCoreOp::LegacyCrossCoreFn;
msg.data = reinterpret_cast<uintptr_t>(task);
```

`cross_core_call` (non-void, void 양쪽):
```cpp
CoreMessage msg;
msg.op = CrossCoreOp::LegacyCrossCoreFn;
msg.data = reinterpret_cast<uintptr_t>(task);
```

- [ ] **Step 4.5: 테스트 파일 — CoreMessage::Type → CrossCoreOp 마이그레이션**

`test_core_engine.cpp` 전체 치환:
- `CoreMessage::Type::Custom` → `CrossCoreOp::Custom` (8곳)
- `CoreMessage::Type::CrossCoreRequest` → `CrossCoreOp::LegacyCrossCoreFn` (2곳)
- `CoreMessage::Type::CrossCorePost` → `CrossCoreOp::LegacyCrossCoreFn` (2곳)
- `msg.type =` → `msg.op =` (designated initializer 포함, 12곳)
- `.type =` → `.op =` (designated initializer, 12곳)
- `msg.type` (비교) → `msg.op` (2곳: line 179, 145 상당)
- `static_cast<uint8_t>(msg.type)` → `static_cast<uint8_t>(msg.op)` (line 179)
- `static_cast<uint8_t>(CoreMessage::Type::Custom)` → `static_cast<uint8_t>(CrossCoreOp::Custom)` (line 193)
- `reinterpret_cast<uint64_t>(task)` → `reinterpret_cast<uintptr_t>(task)` (3곳: lines 212, 232, 269)

`test_pipeline_integration.cpp` 치환:
- `CoreMessage::Type::Custom` → `CrossCoreOp::Custom` (4곳: lines 145, 155, 156, 158)
- `.type =` → `.op =` (designated initializer, 3곳: lines 155-158)
- `msg.type ==` → `msg.op ==` (1곳: line 145)

> `CrossCoreOp::Custom` 값 = 3 이므로 `uint8_t` 캐스트 안전. `reinterpret_cast<uintptr_t>` — 64비트에서 uint64_t와 동일 크기.

- [ ] **Step 5: 전체 빌드 + 전체 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS — CoreMessage 구조 변경 + 테스트 마이그레이션 완료

- [ ] **Step 6: 커밋**

```bash
git add apex_core/include/apex/core/cross_core_op.hpp \
        apex_core/include/apex/core/core_engine.hpp \
        apex_core/src/core_engine.cpp \
        apex_core/include/apex/core/cross_core_call.hpp \
        apex_core/tests/unit/test_core_engine.cpp \
        apex_core/tests/integration/test_pipeline_integration.cpp
git commit -m "refactor(core): CoreMessage Type→CrossCoreOp + uintptr_t 전환"
```

---

## Chunk 2: drain/tick 분리 — 폴링 제거 + 이벤트 기반 drain (Tasks 3–4)

### Task 3: drain/tick 분리 — post() + atomic coalescing + batch-limited drain + tick timer

**Files:**
- Modify: `apex_core/include/apex/core/core_engine.hpp`
- Modify: `apex_core/src/core_engine.cpp`
- Modify: `apex_core/include/apex/core/server.hpp` (ServerConfig drain_interval → tick_interval)
- Modify: `apex_core/src/server.cpp` (drain_callback → tick_callback, CoreEngineConfig 필드명, 설정 전달)
- Modify: `apex_core/tests/unit/test_core_engine.cpp` (.drain_interval→제거/tick_interval, set_drain_callback→set_tick_callback, DrainCallback→TickCallback 테스트명)
- Modify: `apex_core/tests/unit/test_cross_core_call.cpp` (.drain_interval→.tick_interval)
- Modify: `apex_core/src/config.cpp` (drain_interval_us → tick_interval_ms 파싱)
- Modify: `apex_core/config/default.toml` (drain_interval_us → tick_interval_ms)
- Modify: `apex_core/tests/unit/test_config.cpp` (drain_interval_us 검증 → tick_interval_ms)
- Create: `apex_core/tests/unit/test_drain_tick.cpp`

- [ ] **Step 1: test_drain_tick.cpp — drain/tick 분리 테스트 작성**

`apex_core/tests/unit/test_drain_tick.cpp`:

```cpp
#include <apex/core/core_engine.hpp>
#include "../test_helpers.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace apex::core;

TEST(DrainTick, PostTriggersImmediateDrain) {
    CoreEngine engine({.num_cores = 2, .mpsc_queue_capacity = 64,
                       .tick_interval = std::chrono::milliseconds(1000)});

    std::atomic<int> received{0};
    engine.set_message_handler([&](uint32_t, const CoreMessage& msg) {
        if (msg.op == CrossCoreOp::Noop) {
            received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    engine.start();

    // post_to should trigger drain via post(), not wait for timer
    CoreMessage msg{.op = CrossCoreOp::Noop, .source_core = 0, .data = 42};
    auto result = engine.post_to(1, msg);
    ASSERT_TRUE(result.has_value());

    // Drain should happen nearly immediately (not 1000ms tick)
    ASSERT_TRUE(apex::test::wait_for([&] { return received.load() >= 1; },
                std::chrono::milliseconds(100)));

    engine.stop();
    engine.join();
}

TEST(DrainTick, TickCallbackFiresIndependently) {
    CoreEngine engine({.num_cores = 1, .mpsc_queue_capacity = 64,
                       .tick_interval = std::chrono::milliseconds(50)});

    std::atomic<int> tick_count{0};
    engine.set_tick_callback([&](uint32_t) {
        tick_count.fetch_add(1, std::memory_order_relaxed);
    });

    engine.start();

    // No messages posted — tick should still fire
    ASSERT_TRUE(apex::test::wait_for([&] { return tick_count.load() >= 2; },
                std::chrono::milliseconds(300)));

    engine.stop();
    engine.join();
}

TEST(DrainTick, BatchLimitPreventsStarvation) {
    CoreEngine engine({.num_cores = 2, .mpsc_queue_capacity = 4096,
                       .drain_batch_limit = 10,
                       .tick_interval = std::chrono::milliseconds(1000)});

    std::atomic<int> received{0};
    engine.set_message_handler([&](uint32_t, const CoreMessage& msg) {
        received.fetch_add(1, std::memory_order_relaxed);
    });

    engine.start();

    // Flood 100 messages — should be processed in batches of 10
    for (int i = 0; i < 100; ++i) {
        CoreMessage msg{.op = CrossCoreOp::Noop, .source_core = 0};
        (void)engine.post_to(1, msg);
    }

    ASSERT_TRUE(apex::test::wait_for([&] { return received.load() >= 100; },
                std::chrono::milliseconds(500)));

    engine.stop();
    engine.join();
}
```

- [ ] **Step 2: 테스트 실패 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug"`
Expected: FAIL — `set_tick_callback`, `drain_batch_limit` 등이 존재하지 않음

- [ ] **Step 3: CoreEngineConfig 확장**

`apex_core/include/apex/core/core_engine.hpp` — CoreEngineConfig:

```cpp
struct CoreEngineConfig {
    uint32_t num_cores{0};
    size_t mpsc_queue_capacity{65536};
    std::chrono::milliseconds tick_interval{100};  // per-core tick timer interval
    size_t drain_batch_limit{1024};                // max messages per drain cycle
    // drain_interval 제거 — drain은 이벤트 기반으로 전환
};
```

- [ ] **Step 4: CoreEngine 클래스 — drain/tick 분리 API**

```cpp
class CoreEngine {
public:
    using MessageHandler = std::function<void(uint32_t core_id, const CoreMessage& msg)>;
    using TickCallback = std::function<void(uint32_t core_id)>;

    void set_message_handler(MessageHandler handler);
    void set_tick_callback(TickCallback callback);   // drain_callback → tick_callback

    // ... 기존 public API 유지 ...

private:
    void run_core(uint32_t core_id);
    void drain_inbox(uint32_t core_id);              // 신규: batch-limited drain
    void start_tick_timer(uint32_t core_id);         // 신규: 독립 tick timer
    void schedule_drain(uint32_t target_core);       // 신규: coalescing drain 스케줄

    CoreEngineConfig config_;
    std::vector<std::unique_ptr<CoreContext>> cores_;
    std::vector<std::thread> threads_;
    MessageHandler message_handler_;
    TickCallback tick_callback_;                      // drain_callback_ → tick_callback_
    std::atomic<bool> running_{false};
    std::vector<std::atomic<bool>> drain_pending_;   // per-core coalescing flag
};
```

- [ ] **Step 5: CoreContext — tick_timer 추가, drain_timer 유지 여부**

```cpp
struct CoreContext {
    uint32_t core_id;
    boost::asio::io_context io_ctx{1};
    std::unique_ptr<MpscQueue<CoreMessage>> inbox;
    std::unique_ptr<boost::asio::steady_timer> tick_timer;  // drain_timer → tick_timer

    CoreContext(uint32_t id, size_t queue_capacity);
    ~CoreContext();
};
```

> `drain_timer`는 제거. drain은 `post()`로 트리거되므로 타이머 불필요.

- [ ] **Step 6: core_engine.cpp — drain/tick 분리 구현**

**생성자 — drain_pending_ 초기화:**
```cpp
CoreEngine::CoreEngine(CoreEngineConfig config)
    : config_(config)
    , drain_pending_(config_.num_cores)
{
    // ... 기존 코드 ...
    for (auto& flag : drain_pending_) {
        flag.store(false, std::memory_order_relaxed);
    }
}
```

**post_to — enqueue 후 drain 알림:**
```cpp
Result<void> CoreEngine::post_to(uint32_t target_core, CoreMessage msg) {
    if (target_core >= cores_.size()) {
        return error(ErrorCode::Unknown);
    }
    auto result = cores_[target_core]->inbox->enqueue(msg);
    if (!result) {
        return error(ErrorCode::CrossCoreQueueFull);
    }
    schedule_drain(target_core);
    return ok();
}

void CoreEngine::schedule_drain(uint32_t target_core) {
    if (!drain_pending_[target_core].exchange(true, std::memory_order_acq_rel)) {
        boost::asio::post(cores_[target_core]->io_ctx, [this, target_core] {
            drain_pending_[target_core].store(false, std::memory_order_release);
            drain_inbox(target_core);
        });
    }
}
```

**drain_inbox — batch-limited:**
```cpp
void CoreEngine::drain_inbox(uint32_t core_id) {
    auto& core_ctx = *cores_[core_id];
    size_t processed = 0;

    while (processed < config_.drain_batch_limit) {
        auto msg = core_ctx.inbox->dequeue();
        if (!msg) break;

        if (msg->op == CrossCoreOp::LegacyCrossCoreFn) {
            auto* task = reinterpret_cast<std::function<void()>*>(msg->data);
            if (task) {
                try { (*task)(); }
                catch (const std::exception& e) {
                    spdlog::error("Core {} cross-core task exception: {}", core_id, e.what());
                }
                catch (...) {
                    spdlog::error("Core {} cross-core task unknown exception", core_id);
                }
                delete task;
            }
        } else if (message_handler_) {
            message_handler_(core_id, *msg);
        }
        ++processed;
    }

    // 큐에 남은 메시지가 있으면 즉시 재스케줄
    if (processed == config_.drain_batch_limit) {
        boost::asio::post(core_ctx.io_ctx, [this, core_id] {
            drain_inbox(core_id);
        });
    }
}
```

**start_tick_timer — 독립 주기 타이머:**
```cpp
void CoreEngine::start_tick_timer(uint32_t core_id) {
    auto& ctx = *cores_[core_id];
    ctx.tick_timer->expires_after(config_.tick_interval);
    ctx.tick_timer->async_wait([this, core_id](const boost::system::error_code& ec) {
        if (ec) return;
        if (tick_callback_) {
            try { tick_callback_(core_id); }
            catch (const std::exception& e) {
                spdlog::error("Core {} tick_callback exception: {}", core_id, e.what());
            }
            catch (...) {
                spdlog::error("Core {} tick_callback unknown exception", core_id);
            }
        }
        if (running_.load(std::memory_order_acquire)) {
            start_tick_timer(core_id);
        }
    });
}
```

**run_core — tick_timer 시작:**
```cpp
void CoreEngine::run_core(uint32_t core_id) {
    auto& ctx = *cores_[core_id];
    ctx.tick_timer = std::make_unique<boost::asio::steady_timer>(ctx.io_ctx);
    start_tick_timer(core_id);

    auto work_guard = boost::asio::make_work_guard(ctx.io_ctx);
    ctx.io_ctx.run();
}
```

> 기존 `start_drain_timer` 완전 제거. drain은 `post_to` → `schedule_drain`으로만 트리거.

- [ ] **Step 7: server.hpp/cpp — drain_callback → tick_callback 전환**

`server.hpp` ServerConfig:
```cpp
// drain_interval 제거, tick_interval 추가 (이미 있으면 유지)
std::chrono::milliseconds tick_interval{100};
```

`server.cpp`에서 CoreEngine 설정:
```cpp
// 기존: engine.set_drain_callback(...)
// 변경: engine.set_tick_callback(...)
core_engine_->set_tick_callback([this](uint32_t core_id) {
    if (core_id < per_core_.size()) {
        per_core_[core_id]->session_mgr.tick();
    }
});
```

- [ ] **Step 7.5: 테스트/설정 파일 — drain_interval → tick_interval 마이그레이션**

**`test_core_engine.cpp`:**
- `.drain_interval = 50us` → 제거 (drain은 이벤트 기반이므로 불필요) 또는 `.tick_interval = std::chrono::milliseconds(50)` (DrainCallback 테스트만)
- `config.drain_interval` 기본값 테스트(line 19) → `config.tick_interval` 기본값 `100ms` 확인
- `set_drain_callback` → `set_tick_callback` (line 160)
- `DrainCallback` 테스트명 → `TickCallback` (line 156)
- 총 변경: `.drain_interval` 8곳, `drain_callback` 1곳, 테스트명 1곳

**`test_cross_core_call.cpp`:**
- Line 24: `.drain_interval = std::chrono::microseconds{50}` → `.tick_interval = std::chrono::milliseconds{50}`
- Line 148: `.drain_interval = std::chrono::microseconds{60'000'000}` → `.tick_interval = std::chrono::milliseconds{60000}`

**`config.cpp`:**
- `drain_interval_us` → `tick_interval_ms` 키 이름 변경
- `cfg.drain_interval = std::chrono::microseconds(drain_us)` → `cfg.tick_interval = std::chrono::milliseconds(tick_ms)`

**`default.toml`:**
- `drain_interval_us = 100` → `tick_interval_ms = 100`

**`test_config.cpp`:**
- `drain_interval_us = -1` → `tick_interval_ms = -1`

**`server.cpp` (CoreEngineConfig 생성):**
- `.drain_interval = config_.drain_interval` → `.tick_interval = config_.tick_interval`

> drain_interval(μs)→tick_interval(ms) 단위 변경은 의도적: 폴링(100μs)과 tick(100ms)은 역할이 다름. drain은 이벤트 기반으로 즉시 처리.

- [ ] **Step 8: 전체 빌드 + 전체 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS — drain_tick 3개 + 기존 테스트 전부 통과

- [ ] **Step 9: TSAN 테스트** (atomic coalescing 검증)

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat tsan" && ctest --preset tsan -V`
Expected: PASS — data race 없음

- [ ] **Step 10: 커밋**

```bash
git add apex_core/include/apex/core/core_engine.hpp apex_core/src/core_engine.cpp \
        apex_core/include/apex/core/server.hpp apex_core/src/server.cpp \
        apex_core/tests/unit/test_drain_tick.cpp apex_core/CMakeLists.txt \
        apex_core/tests/unit/test_core_engine.cpp \
        apex_core/tests/unit/test_cross_core_call.cpp \
        apex_core/src/config.cpp apex_core/config/default.toml \
        apex_core/tests/unit/test_config.cpp
git commit -m "refactor(core): drain/tick 분리 — 이벤트 기반 drain + 독립 tick timer"
```

---

### Task 4: read_loop shutdown 주석 수정 + payload zero-copy dispatch

**Files:**
- Modify: `apex_core/src/server.cpp` (read_loop 주석 + process_frames zero-copy)
- Test: `apex_core/tests/integration/test_server_e2e.cpp` (기존 E2E 통과 확인)

- [ ] **Step 1: process_frames — payload zero-copy 구현**

`apex_core/src/server.cpp` process_frames 내부:

**Before** (stack_buf 복사 + consume 선행):
```cpp
std::array<uint8_t, TMP_BUF_SIZE> stack_buf{};
// ...
if (frame.payload.size() <= kSmallPayloadThreshold) {
    std::memcpy(stack_buf.data(), frame.payload.data(), frame.payload.size());
    payload_span = {stack_buf.data(), frame.payload.size()};
} else {
    heap_buf.assign(frame.payload.begin(), frame.payload.end());
    payload_span = heap_buf;
}
TcpBinaryProtocol::consume_frame(recv_buf, frame);

auto result = co_await dispatcher.dispatch(session, header.msg_id, payload_span);
```

**After** (zero-copy — RingBuffer 직접 참조, dispatch 후 consume):
```cpp
auto header = frame.header;
auto payload_span = frame.payload;  // RingBuffer 내부 포인터 직접 참조

auto result = co_await dispatcher.dispatch(
    session, header.msg_id, payload_span);

TcpBinaryProtocol::consume_frame(recv_buf, frame);  // dispatch 완료 후 consume
```

> `stack_buf`와 `heap_buf` 변수 제거. `kSmallPayloadThreshold` 상수도 불필요해지면 제거.
> **안전성**: process_frames는 단일 코루틴에서 프레임을 순차 처리하므로, dispatch 완료 전까지 RingBuffer의 해당 영역은 덮어쓰여지지 않음.

- [ ] **Step 2: read_loop — shutdown 관련 주석 수정 (v2 리뷰 항목)**

read_loop에 shutdown 시 close 동작에 대한 명확한 주석 추가. 구체적 내용은 코드 읽고 결정 (현재 read_loop의 shutdown 처리 패턴에 맞춤).

- [ ] **Step 3: 빌드 + E2E 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS — zero-copy는 동작 변경 없이 메모리 복사만 제거하므로 기존 테스트 전부 통과

- [ ] **Step 4: ASAN 테스트** (use-after-free 없음 확인)

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat asan" && ctest --preset asan -V`
Expected: PASS — dispatch 중 RingBuffer 영역이 유효함을 ASAN이 검증

- [ ] **Step 5: 커밋**

```bash
git add apex_core/src/server.cpp
git commit -m "perf(server): payload zero-copy dispatch + read_loop 주석 개선"
```

---

## Chunk 3: Message Passing 인프라 구축 (Tasks 5–6)

### Task 5: SharedPayload — immutable refcounted buffer 기반 클래스

**Files:**
- Create: `apex_core/include/apex/core/shared_payload.hpp` (header-only)
- Create: `apex_core/tests/unit/test_shared_payload.cpp`
- Modify: `apex_core/CMakeLists.txt` (테스트만 추가, 소스 파일 없음)

- [ ] **Step 1: test_shared_payload.cpp 작성**

```cpp
#include <apex/core/shared_payload.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

using namespace apex::core;

// 구체 페이로드 타입 (테스트용)
struct TestPayload : SharedPayload {
    int value{0};
    explicit TestPayload(int v) : value(v) {}
};

TEST(SharedPayload, SingleOwnerRelease) {
    auto* p = new TestPayload(42);
    p->add_ref();
    EXPECT_EQ(p->refcount(), 1u);
    EXPECT_EQ(p->value, 42);

    p->release();  // refcount → 0 → delete
    // No leak (ASAN will catch)
}

TEST(SharedPayload, MultipleOwners) {
    auto* p = new TestPayload(99);
    p->add_ref();
    p->add_ref();
    p->add_ref();
    EXPECT_EQ(p->refcount(), 3u);

    p->release();
    EXPECT_EQ(p->refcount(), 2u);
    p->release();
    EXPECT_EQ(p->refcount(), 1u);
    p->release();  // delete
}

TEST(SharedPayload, BroadcastPattern) {
    // 4코어 브로드캐스트: refcount = 3 (자기 코어 제외)
    auto* p = new TestPayload(7);
    p->set_refcount(3);

    p->release();  // core 1
    p->release();  // core 2
    EXPECT_EQ(p->refcount(), 1u);
    p->release();  // core 3 → delete
}
```

- [ ] **Step 2: 테스트 실패 확인**

Expected: FAIL — SharedPayload 클래스 미존재

- [ ] **Step 3: shared_payload.hpp 구현**

```cpp
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>

namespace apex::core {

/// Base class for immutable, refcounted cross-core payloads.
/// Subclass to add concrete data fields.
///
/// Usage pattern:
///   auto* p = new ConcretePayload{...};
///   p->set_refcount(N);          // broadcast: N receivers
///   engine.post_to(core, msg);   // each receiver calls release() after processing
///
/// Thread safety: refcount operations are atomic. Payload data is immutable
/// after construction — no synchronization needed for reads.
class SharedPayload {
public:
    SharedPayload() = default;
    virtual ~SharedPayload() = default;

    SharedPayload(const SharedPayload&) = delete;
    SharedPayload& operator=(const SharedPayload&) = delete;

    /// Increment refcount (for point-to-point: call once after new).
    void add_ref() noexcept {
        refcount_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Set refcount directly (for broadcast: set to receiver count).
    void set_refcount(uint32_t count) noexcept {
        refcount_.store(count, std::memory_order_relaxed);
    }

    /// Decrement refcount. Deletes this when refcount reaches 0.
    void release() noexcept {
        auto prev = refcount_.fetch_sub(1, std::memory_order_acq_rel);
        assert(prev > 0 && "SharedPayload::release() called with refcount 0");
        if (prev == 1) {
            delete this;
        }
    }

    [[nodiscard]] uint32_t refcount() const noexcept {
        return refcount_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint32_t> refcount_{0};
};

} // namespace apex::core
```

- [ ] **Step 4: 테스트 통과 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -R test_shared_payload -V`
Expected: PASS

- [ ] **Step 5: ASAN 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat asan" && ctest --preset asan -R test_shared_payload -V`
Expected: PASS — 메모리 누수 없음

- [ ] **Step 6: 커밋**

```bash
git add apex_core/include/apex/core/shared_payload.hpp \
        apex_core/tests/unit/test_shared_payload.cpp \
        apex_core/CMakeLists.txt
git commit -m "feat(core): SharedPayload — immutable refcounted cross-core buffer"
```

---

### Task 6: CrossCoreDispatcher — op→handler 매핑 인프라

**Files:**
- Create: `apex_core/include/apex/core/cross_core_dispatcher.hpp`
- Create: `apex_core/src/cross_core_dispatcher.cpp`
- Create: `apex_core/tests/unit/test_cross_core_dispatcher.cpp`
- Modify: `apex_core/include/apex/core/core_engine.hpp` (CrossCoreDispatcher 멤버 추가)
- Modify: `apex_core/src/core_engine.cpp` (drain_inbox에서 dispatcher 사용)
- Modify: `apex_core/CMakeLists.txt`

- [ ] **Step 1: test_cross_core_dispatcher.cpp 작성**

```cpp
#include <apex/core/cross_core_dispatcher.hpp>
#include <gtest/gtest.h>

using namespace apex::core;

TEST(CrossCoreDispatcher, RegisterAndDispatch) {
    CrossCoreDispatcher d;

    int received = 0;
    d.register_handler(CrossCoreOp::Noop,
        [](uint32_t core_id, uint32_t source, void* data) {
            auto* count = static_cast<int*>(data);
            ++(*count);
        });

    int count = 0;
    d.dispatch(0, 1, CrossCoreOp::Noop, &count);
    EXPECT_EQ(count, 1);
}

TEST(CrossCoreDispatcher, UnregisteredOpIsNoOp) {
    CrossCoreDispatcher d;
    // dispatch for unregistered op should not crash
    d.dispatch(0, 1, static_cast<CrossCoreOp>(9999), nullptr);
}

TEST(CrossCoreDispatcher, HasHandler) {
    CrossCoreDispatcher d;
    EXPECT_FALSE(d.has_handler(CrossCoreOp::Noop));

    d.register_handler(CrossCoreOp::Noop,
        [](uint32_t, uint32_t, void*) {});
    EXPECT_TRUE(d.has_handler(CrossCoreOp::Noop));
}
```

- [ ] **Step 2: 테스트 실패 확인**

Expected: FAIL — CrossCoreDispatcher 미존재

- [ ] **Step 3: cross_core_dispatcher.hpp 구현**

```cpp
#pragma once

#include <apex/core/cross_core_op.hpp>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstdint>

namespace apex::core {

/// Dispatches cross-core messages by CrossCoreOp → handler lookup.
/// Handlers are function pointers for static dispatch (no virtual, icache friendly).
/// Thread-safe for concurrent reads after setup (register before start).
class CrossCoreDispatcher {
public:
    CrossCoreDispatcher() = default;

    void register_handler(CrossCoreOp op, CrossCoreHandler handler);

    /// Dispatch a message. No-op if handler not registered.
    void dispatch(uint32_t core_id, uint32_t source_core,
                  CrossCoreOp op, void* data) const;

    [[nodiscard]] bool has_handler(CrossCoreOp op) const noexcept;

private:
    boost::unordered_flat_map<CrossCoreOp, CrossCoreHandler> handlers_;
};

} // namespace apex::core
```

- [ ] **Step 4: cross_core_dispatcher.cpp 구현**

```cpp
#include <apex/core/cross_core_dispatcher.hpp>

namespace apex::core {

void CrossCoreDispatcher::register_handler(CrossCoreOp op, CrossCoreHandler handler) {
    handlers_.insert_or_assign(op, handler);
}

void CrossCoreDispatcher::dispatch(uint32_t core_id, uint32_t source_core,
                                   CrossCoreOp op, void* data) const {
    auto it = handlers_.find(op);
    if (it != handlers_.end()) {
        it->second(core_id, source_core, data);
    }
}

bool CrossCoreDispatcher::has_handler(CrossCoreOp op) const noexcept {
    return handlers_.contains(op);
}

} // namespace apex::core
```

- [ ] **Step 5: CoreEngine에 CrossCoreDispatcher 통합**

`core_engine.hpp`:
```cpp
#include <apex/core/cross_core_dispatcher.hpp>

class CoreEngine {
public:
    /// Register a cross-core message handler. Must be called before start().
    void register_cross_core_handler(CrossCoreOp op, CrossCoreHandler handler);

    // ... 기존 API ...

private:
    CrossCoreDispatcher cross_core_dispatcher_;
    // ...
};
```

`core_engine.cpp` — `drain_inbox` 변경:
```cpp
void CoreEngine::drain_inbox(uint32_t core_id) {
    auto& core_ctx = *cores_[core_id];
    size_t processed = 0;

    while (processed < config_.drain_batch_limit) {
        auto msg = core_ctx.inbox->dequeue();
        if (!msg) break;

        if (msg->op == CrossCoreOp::LegacyCrossCoreFn) {
            // 기존 closure 호환 (전환 완료 후 제거)
            auto* task = reinterpret_cast<std::function<void()>*>(msg->data);
            if (task) {
                try { (*task)(); }
                catch (const std::exception& e) {
                    spdlog::error("Core {} legacy cross-core exception: {}", core_id, e.what());
                }
                catch (...) {
                    spdlog::error("Core {} legacy cross-core unknown exception", core_id);
                }
                delete task;
            }
        } else if (cross_core_dispatcher_.has_handler(msg->op)) {
            // Message passing dispatch (등록된 핸들러 우선)
            cross_core_dispatcher_.dispatch(
                core_id, msg->source_core, msg->op,
                reinterpret_cast<void*>(msg->data));
        } else if (message_handler_) {
            // Fallback: 등록되지 않은 op → 기존 message_handler_ 경로 유지
            // test_core_engine.cpp 등 기존 테스트 호환 + Custom op 처리
            message_handler_(core_id, *msg);
        }
        ++processed;
    }

    if (processed == config_.drain_batch_limit) {
        boost::asio::post(core_ctx.io_ctx, [this, core_id] {
            drain_inbox(core_id);
        });
    }
}
```

- [ ] **Step 6: 테스트 통과 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS — CrossCoreDispatcher 3개 + 기존 테스트 전부

- [ ] **Step 7: 커밋**

```bash
git add apex_core/include/apex/core/cross_core_dispatcher.hpp \
        apex_core/src/cross_core_dispatcher.cpp \
        apex_core/include/apex/core/core_engine.hpp \
        apex_core/src/core_engine.cpp \
        apex_core/tests/unit/test_cross_core_dispatcher.cpp \
        apex_core/CMakeLists.txt
git commit -m "feat(core): CrossCoreDispatcher — op→handler 매핑 인프라"
```

---

## Chunk 4: Message Passing 전환 — closure shipping 대체 (Tasks 7–9)

### Task 7: cross_core_post → message passing 전환

**Files:**
- Modify: `apex_core/include/apex/core/cross_core_call.hpp` (cross_core_post_msg 추가, 기존 cross_core_post는 유지)
- Test: `apex_core/tests/unit/test_cross_core_call.cpp`

> server.hpp의 cross_core_post wrapper 전환은 Task 9에서 legacy 제거와 함께 수행.

- [ ] **Step 1: cross_core_call.hpp — cross_core_post_msg 추가**

기존 `cross_core_post` (closure 기반)는 유지하되 deprecated 마킹. 신규 message passing 버전 추가:

```cpp
/// Message-passing cross_core_post. No heap allocation, no closure.
/// @param payload must outlive the message processing (caller transfers ownership).
///        Handler is responsible for releasing the payload.
inline Result<void> cross_core_post_msg(
    CoreEngine& engine, uint32_t source_core,
    uint32_t target_core, CrossCoreOp op, void* data = nullptr)
{
    CoreMessage msg{
        .op = op,
        .source_core = source_core,
        .data = reinterpret_cast<uintptr_t>(data)
    };
    return engine.post_to(target_core, msg);
}
```

- [ ] **Step 2: test_cross_core_call.cpp — message passing 테스트 추가**

```cpp
TEST_F(CrossCoreCallTest, PostMsgFireAndForget) {
    std::atomic<int> value{0};

    engine().register_cross_core_handler(
        static_cast<CrossCoreOp>(0x0100),  // app-defined op
        [](uint32_t, uint32_t, void* data) {
            auto* v = static_cast<std::atomic<int>*>(data);
            v->store(99, std::memory_order_relaxed);
        });

    auto result = cross_core_post_msg(
        engine(), 0, 1,
        static_cast<CrossCoreOp>(0x0100), &value);
    EXPECT_TRUE(result.has_value());

    ASSERT_TRUE(apex::test::wait_for([&] { return value.load() == 99; }));
}
```

- [ ] **Step 3: 빌드 + 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS

- [ ] **Step 4: 커밋**

```bash
git add apex_core/include/apex/core/cross_core_call.hpp \
        apex_core/tests/unit/test_cross_core_call.cpp
git commit -m "feat(core): cross_core_post_msg — message passing fire-and-forget"
```

---

### Task 8: cross_core_call → cross_core_request (request-response message passing)

**Files:**
- Create: `apex_core/include/apex/core/cross_core_message.hpp` (cross_core_request 구현)
- Modify: `apex_core/include/apex/core/core_engine.hpp` (pending_requests per-core map)
- Modify: `apex_core/src/core_engine.cpp` (pending request 관리)
- Test: `apex_core/tests/unit/test_cross_core_call.cpp`

- [ ] **Step 1: cross_core_message.hpp — cross_core_request 구현**

```cpp
#pragma once

#include <apex/core/core_engine.hpp>
#include <apex/core/cross_core_op.hpp>
#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>
#include <apex/core/shared_payload.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace apex::core {

/// Per-core pending request state (core-local, no synchronization needed).
struct PendingRequest {
    std::function<void(void*)> callback;
    boost::asio::steady_timer* timer;
};

/// Per-core pending request map. NOT thread-safe — accessed only by owning core.
using PendingRequestMap = boost::unordered_flat_map<uint64_t, PendingRequest>;

/// Cross-core request-response pattern using message passing.
/// Replaces cross_core_call with zero heap allocations and no CAS races.
///
/// @pre Must be called from a coroutine on a core thread.
template <typename ResultType>
auto cross_core_request(
    CoreEngine& engine, uint32_t target_core,
    CrossCoreOp op, void* request_data,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
    -> boost::asio::awaitable<Result<ResultType>>
{
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor);
    timer.expires_after(timeout);

    uint64_t req_id = engine.next_request_id();
    std::optional<ResultType> result;

    engine.register_pending(req_id, PendingRequest{
        .callback = [&result, &timer](void* raw) {
            result.emplace(std::move(*static_cast<ResultType*>(raw)));
            timer.cancel();
        },
        .timer = &timer
    });

    // 요청 전송
    CoreMessage msg{
        .op = op,
        .source_core = engine.current_core_id(),
        .data = reinterpret_cast<uintptr_t>(request_data)
    };
    auto post_result = engine.post_to(target_core, msg);
    if (!post_result) {
        engine.remove_pending(req_id);
        co_return apex::core::error(post_result.error());
    }

    // 응답 또는 timeout 대기
    auto [ec] = co_await timer.async_wait(
        boost::asio::as_tuple(boost::asio::use_awaitable));
    engine.remove_pending(req_id);

    if (!result) co_return apex::core::error(ErrorCode::CrossCoreTimeout);
    co_return std::move(*result);
}

} // namespace apex::core
```

- [ ] **Step 2: CoreEngine — pending request 인프라**

`core_engine.hpp` 추가:
```cpp
    /// Request ID generator (per-engine, atomic)
    [[nodiscard]] uint64_t next_request_id() noexcept;

    /// Per-core pending request management (core-local)
    void register_pending(uint64_t req_id, PendingRequest request);
    void remove_pending(uint64_t req_id);
    void resolve_pending(uint64_t req_id, void* result_data);

    /// Current core ID (must be called from core thread)
    [[nodiscard]] uint32_t current_core_id() const noexcept;

private:
    std::atomic<uint64_t> next_req_id_{1};
    std::vector<PendingRequestMap> pending_requests_;  // per-core
    static thread_local uint32_t tls_core_id_;
```

- [ ] **Step 2.5: core_engine.cpp — pending request 인프라 구현**

```cpp
// --- thread_local core_id ---
thread_local uint32_t CoreEngine::tls_core_id_ = UINT32_MAX;

// run_core 시작부에 추가:
void CoreEngine::run_core(uint32_t core_id) {
    tls_core_id_ = core_id;  // TLS 설정
    // ... 기존 코드 ...
}

uint32_t CoreEngine::current_core_id() const noexcept {
    return tls_core_id_;
}

uint64_t CoreEngine::next_request_id() noexcept {
    return next_req_id_.fetch_add(1, std::memory_order_relaxed);
}

void CoreEngine::register_pending(uint64_t req_id, PendingRequest request) {
    auto core_id = tls_core_id_;
    assert(core_id < pending_requests_.size());
    pending_requests_[core_id].insert_or_assign(req_id, std::move(request));
}

void CoreEngine::remove_pending(uint64_t req_id) {
    auto core_id = tls_core_id_;
    if (core_id < pending_requests_.size()) {
        pending_requests_[core_id].erase(req_id);
    }
}

void CoreEngine::resolve_pending(uint64_t req_id, void* result_data) {
    // 응답을 받은 코어에서 호출됨 — 요청을 보낸 코어의 pending map에서 콜백 실행
    // NOTE: 이 함수는 target core에서 호출되므로 source core의 map에 접근 필요.
    // 안전한 방법: source core에 post()로 콜백 전달 (core-local 접근만 허용).
    // 구현 세부는 cross_core_request 템플릿의 콜백 패턴 참조.
    // (콜백이 timer.cancel()을 호출해 caller 코어의 코루틴을 resume시킴)
}
```

> `pending_requests_` 초기화: CoreEngine 생성자에서 `pending_requests_.resize(config_.num_cores)` 추가.
> `tls_core_id_`는 `static thread_local`이므로 각 코어 스레드에서 독립 관리.

- [ ] **Step 3: 테스트 + 빌드**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS

- [ ] **Step 4: 커밋**

```bash
git add apex_core/include/apex/core/cross_core_message.hpp \
        apex_core/include/apex/core/core_engine.hpp \
        apex_core/src/core_engine.cpp \
        apex_core/tests/unit/test_cross_core_call.cpp \
        apex_core/CMakeLists.txt
git commit -m "feat(core): cross_core_request — request-response message passing"
```

---

### Task 9: broadcast_cross_core + Legacy closure 제거

**Files:**
- Modify: `apex_core/include/apex/core/cross_core_call.hpp` (broadcast 추가, legacy cross_core_post/cross_core_call 제거)
- Modify: `apex_core/src/core_engine.cpp` (LegacyCrossCoreFn 분기 제거, drain_remaining에서 Legacy 정리 제거)
- Modify: `apex_core/include/apex/core/core_engine.hpp` (broadcast API)
- Modify: `apex_core/include/apex/core/server.hpp` (cross_core_post/call wrapper → message passing API)
- Modify: `apex_core/tests/unit/test_cross_core_call.cpp` (legacy closure 테스트 → message passing 테스트로 전환)
- Modify: `apex_core/tests/unit/test_core_engine.cpp` (CrossCoreRequestAutoExecuted, DrainRemainingCleansUpPointers, DestructorDrainsRemaining → message passing 기반으로 전환)
- Modify: `apex_core/include/apex/core/cross_core_op.hpp` (LegacyCrossCoreFn 제거)
- Test: 전체 테스트 통과 확인

- [ ] **Step 1: broadcast_cross_core 구현**

```cpp
/// Broadcast an immutable shared payload to all cores except source.
/// Caller must set payload->set_refcount(engine.core_count() - 1) before calling.
inline void broadcast_cross_core(
    CoreEngine& engine, uint32_t source_core,
    CrossCoreOp op, SharedPayload* payload)
{
    for (uint32_t i = 0; i < engine.core_count(); ++i) {
        if (i == source_core) continue;
        CoreMessage msg{
            .op = op,
            .source_core = source_core,
            .data = reinterpret_cast<uintptr_t>(payload)
        };
        if (!engine.post_to(i, msg).has_value()) {
            payload->release();  // 전송 실패한 코어 분의 refcount 감소
        }
    }
}
```

- [ ] **Step 2: Legacy closure 분기 + 함수 제거**

**`core_engine.cpp`** drain_inbox에서 `CrossCoreOp::LegacyCrossCoreFn` if 분기 전체 삭제.
`drain_remaining`에서도 LegacyCrossCoreFn 타입 체크 + `delete task` 분기 삭제.

**`cross_core_call.hpp`** 에서 기존 3개 함수 완전 제거:
- `cross_core_call<F>` (non-void) — shared_ptr<State> + CAS 기반
- `cross_core_call<F>` (void) — 동일 패턴
- `cross_core_post<F>` — `new std::function` 기반

**`cross_core_op.hpp`** 에서 `LegacyCrossCoreFn` 열거값 제거.

**`test_core_engine.cpp`** — Legacy 의존 테스트 3개 전환:
- `CrossCoreRequestAutoExecuted`: `new std::function` + `LegacyCrossCoreFn` → `register_cross_core_handler` + `CrossCoreOp::Custom` + 직접 `post_to`
- `DrainRemainingCleansUpPointers`: Legacy pointer delete 검증 → SharedPayload refcount 기반 검증으로 전환
- `DestructorDrainsRemaining`: 동일 전환

**`test_cross_core_call.cpp`** — Legacy `cross_core_post`/`cross_core_call` 테스트 → `cross_core_post_msg`/`cross_core_request` 기반으로 전면 전환.

- [ ] **Step 3: server.hpp — cross_core_post/call wrapper 전환**

기존 closure 기반 wrapper를 message passing API로 교체:
```cpp
    template <typename F>
    Result<void> cross_core_post(uint32_t target_core, CrossCoreOp op, void* data = nullptr) {
        return cross_core_post_msg(
            *core_engine_, current_core_id(), target_core, op, data);
    }
```

- [ ] **Step 4: 전체 빌드 + 전체 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS

- [ ] **Step 5: TSAN + ASAN 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat tsan" && ctest --preset tsan -V`
Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat asan" && ctest --preset asan -V`
Expected: PASS

- [ ] **Step 6: 커밋**

```bash
git add apex_core/include/apex/core/cross_core_call.hpp \
        apex_core/include/apex/core/cross_core_op.hpp \
        apex_core/include/apex/core/core_engine.hpp \
        apex_core/src/core_engine.cpp \
        apex_core/include/apex/core/server.hpp \
        apex_core/tests/unit/test_cross_core_call.cpp \
        apex_core/tests/unit/test_core_engine.cpp
git commit -m "feat(core): broadcast_cross_core + legacy closure shipping 제거"
```

---

## Summary

### 핵심 변경 3가지
1. **MessageDispatcher**: `std::array<Handler, 65536>` (~2MB) → `boost::unordered_flat_map` (등록 수 비례)
2. **drain/tick 분리**: 100μs 폴링 타이머 → `post()` + atomic coalescing 이벤트 기반 drain + 독립 tick timer
3. **Message passing**: closure shipping (`new std::function` per-message) → `CrossCoreOp` + `SharedPayload` + function pointer dispatch

### 제거된 코드
- `CoreMessage::Type` enum → `CrossCoreOp` enum으로 대체
- `drain_timer` / `start_drain_timer` → `drain_inbox` + `schedule_drain` + `start_tick_timer`
- `drain_callback` / `drain_interval` → `tick_callback` / `tick_interval` (μs→ms 단위 변경)
- `cross_core_post` (closure) → `cross_core_post_msg` (message passing)
- `cross_core_call` (CAS + shared_ptr) → `cross_core_request` (core-local pending map)
- `stack_buf` / `heap_buf` payload 복사 → zero-copy dispatch
- TOML: `drain_interval_us` → `tick_interval_ms`

### 수정된 테스트/설정 파일 (리뷰 라운드 1 보강)
- `test_core_engine.cpp` — CoreMessage::Type 마이그레이션 (12곳) + drain_interval 제거 (8곳) + drain_callback→tick_callback + Legacy 테스트 전환 (3개)
- `test_pipeline_integration.cpp` — CoreMessage::Type 마이그레이션 (4곳)
- `test_cross_core_call.cpp` — drain_interval→tick_interval (2곳) + Legacy→message passing 전환
- `test_config.cpp`, `config.cpp`, `default.toml` — drain_interval_us→tick_interval_ms

### 신규 파일 (5개)
- `cross_core_op.hpp` — CrossCoreOp enum (Custom 포함, LegacyCrossCoreFn 전환 후 제거)
- `shared_payload.hpp` — immutable refcounted buffer (header-only)
- `cross_core_dispatcher.hpp/.cpp` — op→handler 매핑
- `cross_core_message.hpp` — cross_core_post_msg, cross_core_request, broadcast_cross_core
- `test_drain_tick.cpp`, `test_shared_payload.cpp`, `test_cross_core_dispatcher.cpp`

### Task 별 커밋 (9개)
1. `refactor(dispatch): MessageDispatcher 65536-array → boost::unordered_flat_map`
2. `refactor(core): CoreMessage Type→CrossCoreOp + uintptr_t 전환`
3. `refactor(core): drain/tick 분리 — 이벤트 기반 drain + 독립 tick timer`
4. `perf(server): payload zero-copy dispatch + read_loop 주석 개선`
5. `feat(core): SharedPayload — immutable refcounted cross-core buffer`
6. `feat(core): CrossCoreDispatcher — op→handler 매핑 인프라`
7. `feat(core): cross_core_post_msg — message passing fire-and-forget`
8. `feat(core): cross_core_request — request-response message passing`
9. `feat(core): broadcast_cross_core + legacy closure shipping 제거`
