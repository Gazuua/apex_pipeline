# Phase 5.5 Tier 0.5: 에러 타입 통일 — 구현 계획

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** DispatchError/QueueError를 ErrorCode로 흡수하고, dispatch/enqueue/post_to/async_send 반환 타입을 Result<void>로 통일하여 프레임워크 전체에서 단일 에러 채널을 확립한다.

**Architecture:** ErrorCode enum에 2개 코드 추가 (HandlerException, SendFailed) → 하위 계층부터 상위 계층 순서로 반환 타입 전환 (MpscQueue → CoreEngine → cross_core → dispatch → async_send → callers). 각 계층이 자기 수준의 의미를 부여하는 **계층별 에러 매핑** 패턴 적용. MpscQueue는 범용 `BufferFull`, CoreEngine은 `CrossCoreQueueFull`로 매핑.

**Tech Stack:** C++23 `std::expected`, Boost.Asio awaitable, GTest

**v6 계획서 참조**: `docs/apex_common/plans/20260311_204613_phase5_5_v6.md` § 3.3 (에러 타입 통일), § 3.2 (async_send), § 6 Tier 0.5 작업 목록

---

## File Structure

### Modified Files

| File | Change |
|------|--------|
| `apex_core/include/apex/core/error_code.hpp` | `HandlerException = 11`, `SendFailed = 12` 추가 + `error_code_name()` 확장 |
| `apex_core/include/apex/core/mpsc_queue.hpp` | `QueueError` enum 제거, `enqueue()` → `Result<void>` |
| `apex_core/include/apex/core/message_dispatcher.hpp` | `DispatchError` enum 제거, `dispatch()` → `awaitable<Result<void>>` |
| `apex_core/src/message_dispatcher.cpp` | dispatch 내부에서 ErrorCode 직접 반환 |
| `apex_core/include/apex/core/core_engine.hpp` | `post_to()` → `Result<void>` |
| `apex_core/src/core_engine.cpp` | post_to 구현 변경, broadcast `(void)` 유지 |
| `apex_core/include/apex/core/cross_core_call.hpp` | `cross_core_post()` → `Result<void>`, cross_core_call 내부 post_to 호출 갱신 |
| `apex_core/include/apex/core/session.hpp` | `async_send`/`async_send_raw` → `awaitable<Result<void>>` |
| `apex_core/src/session.cpp` | async_send/async_send_raw 구현 변경 |
| `apex_core/include/apex/core/server.hpp` | `cross_core_post` wrapper → `Result<void>` |
| `apex_core/src/server.cpp` | process_frames dispatch/async_send 호출 단순화 |
| `apex_core/examples/echo_server.cpp` | async_send 결과 처리 갱신 |
| `apex_core/examples/multicore_echo_server.cpp` | 동일 |
| `apex_core/examples/chat_server.cpp` | `if (!co_await s->async_send(...))` → `.has_value()` |
| `apex_core/tests/unit/test_error_propagation.cpp` | 신규 ErrorCode 테스트 추가 |
| `apex_core/tests/unit/test_mpsc_queue.cpp` | QueueError → ErrorCode::BufferFull |
| `apex_core/tests/unit/test_message_dispatcher.cpp` | DispatchError → ErrorCode, double-check 패턴 제거 |
| `apex_core/tests/unit/test_service_base.cpp` | DispatchError → ErrorCode, double-check 패턴 제거 |
| `apex_core/tests/unit/test_flatbuffers_dispatch.cpp` | double-check 패턴 (`.value().has_value()`) 제거 |
| `apex_core/tests/unit/test_session.cpp` | bool → Result<void> 검증 |
| `apex_core/tests/unit/test_cross_core_call.cpp` | bool → Result<void> 검증 |
| `apex_core/tests/integration/test_pipeline_integration.cpp` | DispatchError → ErrorCode |
| `apex_core/tests/integration/test_server_e2e.cpp` | HandlerFailed: Unknown → HandlerException |

---

## Chunk 1: ErrorCode 확장 + 하위 계층 에러 타입 전환 + dispatch caller 통합 (Tasks 1–4)

### Task 1: ErrorCode 확장 — 신규 에러 코드 추가

**Files:**
- Modify: `apex_core/include/apex/core/error_code.hpp:9-44`
- Test: `apex_core/tests/unit/test_error_propagation.cpp`

- [ ] **Step 1: test_error_propagation.cpp에 신규 ErrorCode 테스트 추가**

`apex_core/tests/unit/test_error_propagation.cpp` 끝에 추가:

```cpp
TEST(ErrorCode, HandlerExceptionName) {
    EXPECT_EQ(error_code_name(ErrorCode::HandlerException), "HandlerException");
}

TEST(ErrorCode, SendFailedName) {
    EXPECT_EQ(error_code_name(ErrorCode::SendFailed), "SendFailed");
}

TEST(Result, HandlerExceptionError) {
    Result<void> r = error(ErrorCode::HandlerException);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ErrorCode::HandlerException);
}

TEST(Result, SendFailedError) {
    Result<void> r = error(ErrorCode::SendFailed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ErrorCode::SendFailed);
}
```

- [ ] **Step 2: 테스트 실패 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -R test_error_propagation -V`
Expected: FAIL — `HandlerException`, `SendFailed`가 ErrorCode에 없음 (컴파일 에러)

- [ ] **Step 3: error_code.hpp에 신규 에러 코드 추가**

`apex_core/include/apex/core/error_code.hpp` — ErrorCode enum에 추가:

```cpp
enum class ErrorCode : uint16_t {
    Ok = 0,

    // Framework errors (1-999)
    Unknown = 1,
    InvalidMessage = 2,
    HandlerNotFound = 3,
    SessionClosed = 4,
    BufferFull = 5,
    Timeout = 6,
    FlatBuffersVerifyFailed = 7,
    CrossCoreTimeout = 8,
    CrossCoreQueueFull = 9,
    UnsupportedProtocolVersion = 10,
    HandlerException = 11,      // dispatch handler threw exception (absorbs DispatchError::HandlerFailed)
    SendFailed = 12,            // async_send network write failure

    // Application errors (1000+)
    AppError = 1000,
};
```

`error_code_name()` switch에 추가:

```cpp
        case ErrorCode::HandlerException: return "HandlerException";
        case ErrorCode::SendFailed: return "SendFailed";
```

- [ ] **Step 4: 테스트 통과 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -R test_error_propagation -V`
Expected: PASS — 기존 테스트 + 신규 4개 전부 통과

- [ ] **Step 5: 커밋**

```bash
git add apex_core/include/apex/core/error_code.hpp apex_core/tests/unit/test_error_propagation.cpp
git commit -m "feat(error): ErrorCode에 HandlerException, SendFailed 추가"
```

---

### Task 2: MpscQueue — QueueError 제거, enqueue → Result<void>

**Files:**
- Modify: `apex_core/include/apex/core/mpsc_queue.hpp:1-166`
- Test: `apex_core/tests/unit/test_mpsc_queue.cpp`

- [ ] **Step 1: test_mpsc_queue.cpp 에러 검증 코드 변경**

`apex_core/tests/unit/test_mpsc_queue.cpp` 변경:

상단 include 변경 — `result.hpp` 추가:
```cpp
#include <apex/core/mpsc_queue.hpp>
#include <apex/core/result.hpp>   // ErrorCode, Result
#include <gtest/gtest.h>
```

`using namespace` 아래 `using` 추가:
```cpp
using namespace apex::core;
```

`Backpressure_WhenFull` 테스트 (라인 52-60) 변경:
```cpp
TEST(MpscQueue, Backpressure_WhenFull) {
    MpscQueue<int> q(4);
    for (size_t i = 0; i < q.capacity(); ++i) {
        ASSERT_TRUE(q.enqueue(static_cast<int>(i)).has_value());
    }
    auto result = q.enqueue(999);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::BufferFull);
}
```

`BackpressureDrainReuse` 테스트 (라인 84) — `EXPECT_FALSE` 검증 유지 (에러 타입만 변경됨, has_value() 체크는 동일하므로 코드 수정 불필요).

- [ ] **Step 2: 테스트 실패 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -R test_mpsc_queue -V`
Expected: FAIL — `QueueError::Full` 대신 `ErrorCode::BufferFull` 비교 → 컴파일 에러

- [ ] **Step 3: mpsc_queue.hpp 변경 — QueueError 제거, enqueue 반환 타입 변경**

`apex_core/include/apex/core/mpsc_queue.hpp`:

**include 변경** — `<expected>` 제거, `result.hpp` 추가:
```cpp
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>

#include <apex/core/detail/math_utils.hpp>
#include <apex/core/result.hpp>
```

**QueueError enum 삭제** (라인 17-19):
```cpp
// 삭제: enum class QueueError : uint8_t { Full, };
```

**enqueue 선언 변경** (라인 76):
```cpp
    /// Thread-safe. Lock-free. Called by any producer core.
    /// @return ErrorCode::BufferFull if queue is at capacity (backpressure).
    [[nodiscard]] Result<void> enqueue(const T& item);
```

**enqueue 구현 변경** (라인 144-166):
```cpp
template <typename T>
    requires std::is_trivially_copyable_v<T>
Result<void> MpscQueue<T>::enqueue(const T& item) {
    size_t head = head_.load(std::memory_order_relaxed);
    size_t tail = tail_.load(std::memory_order_acquire);
    for (;;) {
        if (head - tail >= capacity_) {
            return error(ErrorCode::BufferFull);
        }

        if (head_.compare_exchange_weak(head, head + 1,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            Slot& slot = slots_[head & mask_];
            slot.data = item;
            slot.ready.store(true, std::memory_order_release);
            return ok();
        }
        // CAS failed — head was updated by compare_exchange_weak.
        // Reload tail: consumer may have advanced since our last read,
        // freeing slots that would otherwise cause a spurious Full return.
        tail = tail_.load(std::memory_order_acquire);
    }
}
```

- [ ] **Step 4: 테스트 통과 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -R test_mpsc_queue -V`
Expected: PASS — 7개 테스트 전부 통과

- [ ] **Step 5: 커밋**

```bash
git add apex_core/include/apex/core/mpsc_queue.hpp apex_core/tests/unit/test_mpsc_queue.cpp
git commit -m "refactor(mpsc): QueueError 제거 → Result<void> + ErrorCode::BufferFull"
```

---

### Task 3: MessageDispatcher — DispatchError 제거, dispatch → Result<void>

**Files:**
- Modify: `apex_core/include/apex/core/message_dispatcher.hpp:1-53`
- Modify: `apex_core/src/message_dispatcher.cpp:1-55`
- Modify: `apex_core/src/server.cpp:383-397` (dispatch 호출부 단순화 — 반환 타입 변경과 동시에 수정해야 빌드 깨짐 방지)
- Test: `apex_core/tests/unit/test_message_dispatcher.cpp`
- Test: `apex_core/tests/unit/test_service_base.cpp`
- Test: `apex_core/tests/unit/test_flatbuffers_dispatch.cpp`
- Test: `apex_core/tests/integration/test_pipeline_integration.cpp` (DispatchError 참조만 — Task 3에서 제거 필수)
- Test: `apex_core/tests/integration/test_server_e2e.cpp`

- [ ] **Step 1: test_message_dispatcher.cpp 변경 — DispatchError 참조 제거**

`apex_core/tests/unit/test_message_dispatcher.cpp` 변경:

**using 선언 변경** (라인 15):
```cpp
// 삭제: using apex::core::DispatchError;
using apex::core::ErrorCode;
```

**RegisterAndDispatch** (라인 46-48) — double-check 제거:
```cpp
    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x0001, {}));
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(called);
```

**DispatchUnknownReturnsError** (라인 52-56):
```cpp
TEST_F(MessageDispatcherTest, DispatchUnknownReturnsError) {
    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x9999, {}));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::HandlerNotFound);
}
```

**PayloadPassedThrough** (라인 67-70):
```cpp
    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x0010, data));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(received, data);
```

**OverwriteHandler** (라인 91-94):
```cpp
    run_coro(io_ctx_, [&]() -> awaitable<void> {
        auto result = co_await d->dispatch(nullptr, 0x0001, {});
        EXPECT_TRUE(result.has_value());
    }());
```

**MultipleHandlers** (라인 125-128):
```cpp
        auto result = run_coro(io_ctx_, d->dispatch(nullptr, i, {}));
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(counts[i], 1);
```

**MaxMsgId** (라인 142-144):
```cpp
    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0xFFFF, {}));
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(called);
```

**HandlerExceptionReturnsHandlerFailed** (라인 148-157) — 이름+검증 변경:
```cpp
TEST_F(MessageDispatcherTest, HandlerExceptionReturnsHandlerException) {
    d->register_handler(0x0001,
        [](SessionPtr, uint16_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
            throw std::runtime_error("test error");
            co_return apex::core::ok();
        });
    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x0001, {}));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::HandlerException);
}
```

**HandlerReturnsErrorCode** (라인 159-168) — double-check 제거:
```cpp
TEST_F(MessageDispatcherTest, HandlerReturnsErrorCode) {
    d->register_handler(0x0001,
        [](SessionPtr, uint16_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
            co_return apex::core::error(apex::core::ErrorCode::Timeout);
        });
    auto result = run_coro(io_ctx_, d->dispatch(nullptr, 0x0001, {}));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::core::ErrorCode::Timeout);
}
```
> **의미 변화 참고**: 기존에는 "dispatch 성공 + handler error"(outer OK, inner error)와 "dispatch 실패"(outer error)가 구분되었으나, 통일 후에는 모두 `Result<void>` 에러로 합쳐진다. server.cpp에서 어차피 에러 프레임을 보내므로 실질적 동작 차이 없음.

- [ ] **Step 2: test_service_base.cpp 변경 — DispatchError 참조 제거 + double-check 제거**

`apex_core/tests/unit/test_service_base.cpp` 변경:

**HandleRegistersAndDispatches** (라인 104-106) — double-check 제거:
```cpp
    auto result = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0001, data));
    EXPECT_TRUE(result.has_value());
    // 삭제: EXPECT_TRUE(result.value().has_value());  ← dispatch가 Result<void> 직접 반환하므로 불필요
    EXPECT_EQ(svc->last_msg_id, 0x0001);
```

**UnregisteredMsgReturnsError** (라인 116-118) — DispatchError → ErrorCode:
```cpp
    auto result = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x9999, {}));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::HandlerNotFound);
```

**BindExternalDispatcher** (라인 158-160) — double-check 제거:
```cpp
    auto result = run_coro(io_ctx, external_dispatcher->dispatch(nullptr, 0x0001, data));
    EXPECT_TRUE(result.has_value());
    // 삭제: EXPECT_TRUE(result.value().has_value());  ← 위와 동일
    EXPECT_EQ(svc->last_msg_id, 0x0001);
```

**MultipleHandlers** (라인 126-131) — `.has_value()` 패턴은 outer/inner 무관하게 유효. **변경 불필요.**

- [ ] **Step 2.5: test_flatbuffers_dispatch.cpp 변경 — double-check 패턴 제거**

`apex_core/tests/unit/test_flatbuffers_dispatch.cpp` 변경:

**RouteTypedMessage** (라인 60-62) — double-check 제거:
```cpp
    auto result = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0010, payload));
    EXPECT_TRUE(result.has_value());
    // 삭제: EXPECT_TRUE(result.value().has_value());  ← dispatch가 Result<void> 직접 반환
```

**RouteInvalidFlatBuffer** (라인 74-78) — 이중 expected → 단일 Result:
```cpp
    auto result = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0010, garbage));
    // route()가 검증 실패 시 ErrorCode::FlatBuffersVerifyFailed 반환
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::FlatBuffersVerifyFailed);
```
> 기존: outer expected OK + inner Result error → 변경 후: Result 직접 error. `EXPECT_TRUE(result.has_value())` (라인 76) 삭제, `.value().has_value()` → `.has_value()`, `.value().error()` → `.error()`.

**RouteAndRawHandlerCoexist** (라인 96-101) — double-check 제거:
```cpp
    auto r1 = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0010, payload));
    EXPECT_TRUE(r1.has_value());
    // 삭제: EXPECT_TRUE(r1.value().has_value());
    auto r2 = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0020, {}));
    EXPECT_TRUE(r2.has_value());
    // 삭제: EXPECT_TRUE(r2.value().has_value());
```

- [ ] **Step 2.6: test_pipeline_integration.cpp — DispatchError 참조 제거**

`apex_core/tests/integration/test_pipeline_integration.cpp` 변경:

**UnknownMessageIdHandledGracefully** (라인 130-133) — DispatchError → ErrorCode:
```cpp
    auto result = run_coro(io_ctx, service->dispatcher().dispatch(nullptr,
        frame->header.msg_id, frame->payload));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::core::ErrorCode::HandlerNotFound);
```

> Task 3에서 `DispatchError` enum이 삭제되므로, 이 참조를 같은 커밋에서 제거해야 빌드 깨짐 방지. `EncodeDecodeDispatch`/`MultiFramePipeline`은 `.has_value()`만 사용하므로 변경 불필요 (dispatch 반환 타입이 바뀌어도 `.has_value()` 호출은 양쪽 모두 유효).

- [ ] **Step 3: 테스트 실패 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug"`
Expected: FAIL — `dispatch()` 반환 타입이 아직 `expected<Result<void>, DispatchError>` → 컴파일 에러

- [ ] **Step 4: message_dispatcher.hpp 변경**

`apex_core/include/apex/core/message_dispatcher.hpp`:

**DispatchError enum 삭제** (라인 17-20):
```cpp
// 삭제: enum class DispatchError : uint8_t { UnknownMessage, HandlerFailed, };
```

**dispatch 반환 타입 변경** (라인 41-42):
```cpp
    [[nodiscard]] boost::asio::awaitable<Result<void>>
    dispatch(SessionPtr session, uint16_t msg_id, std::span<const uint8_t> payload) const;
```

**불필요한 `<expected>` include 제거** (라인 10) — `result.hpp`가 이미 포함:
```cpp
// 삭제: #include <expected>
```

- [ ] **Step 5: message_dispatcher.cpp 변경**

`apex_core/src/message_dispatcher.cpp` — dispatch 구현 변경:

```cpp
boost::asio::awaitable<Result<void>>
MessageDispatcher::dispatch(SessionPtr session, uint16_t msg_id, std::span<const uint8_t> payload) const {
    auto& handler = (*handlers_)[msg_id];
    if (!handler) {
        co_return error(ErrorCode::HandlerNotFound);
    }
    try {
        co_return co_await handler(std::move(session), msg_id, payload);
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
```

- [ ] **Step 6: server.cpp process_frames — dispatch 이중 체크 단순화**

`apex_core/src/server.cpp` (라인 383-397) — dispatch 이중 expected 처리 → 단일 Result:

**Before** (12줄):
```cpp
        auto result = co_await dispatcher.dispatch(
            session, header.msg_id, payload_span);

        if (!result.has_value()) {
            ErrorCode code = (result.error() == DispatchError::UnknownMessage)
                ? ErrorCode::HandlerNotFound
                : ErrorCode::Unknown;
            auto error_frame = ErrorSender::build_error_frame(
                header.msg_id, code);
            (void)co_await session->async_send_raw(error_frame);
        } else if (!result.value().has_value()) {
            auto error_frame = ErrorSender::build_error_frame(
                header.msg_id, result.value().error());
            (void)co_await session->async_send_raw(error_frame);
        }
```

**After** (5줄):
```cpp
        auto result = co_await dispatcher.dispatch(
            session, header.msg_id, payload_span);

        if (!result.has_value()) {
            auto error_frame = ErrorSender::build_error_frame(
                header.msg_id, result.error());
            (void)co_await session->async_send_raw(error_frame);
        }
```

> **주의**: dispatch 반환 타입 변경과 **반드시 같은 커밋**에서 수정해야 함. server.cpp가 dispatch를 호출하므로, 반환 타입만 바꾸고 caller를 안 바꾸면 컴파일이 깨짐.

- [ ] **Step 7: test_server_e2e.cpp — HandlerFailed 에러 코드 기대값 변경**

`apex_core/tests/integration/test_server_e2e.cpp` (라인 301-303):

**Before:**
```cpp
    EXPECT_EQ(err->code(), static_cast<uint16_t>(ErrorCode::Unknown));
```

**After:**
```cpp
    EXPECT_EQ(err->code(), static_cast<uint16_t>(ErrorCode::HandlerException));
```

> handler 예외 시 기존에는 `DispatchError::HandlerFailed` → server.cpp에서 `ErrorCode::Unknown`으로 매핑했지만, 이제 dispatch가 직접 `ErrorCode::HandlerException`을 반환하므로 기대값이 바뀜.

- [ ] **Step 8: 전체 빌드 + 테스트 통과 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS — test_message_dispatcher(9개) + test_service_base + test_server_e2e + 기타 전부 통과

- [ ] **Step 9: 커밋**

```bash
git add apex_core/include/apex/core/message_dispatcher.hpp apex_core/src/message_dispatcher.cpp \
        apex_core/src/server.cpp \
        apex_core/tests/unit/test_message_dispatcher.cpp apex_core/tests/unit/test_service_base.cpp \
        apex_core/tests/unit/test_flatbuffers_dispatch.cpp \
        apex_core/tests/integration/test_pipeline_integration.cpp \
        apex_core/tests/integration/test_server_e2e.cpp
git commit -m "refactor(dispatch): DispatchError 제거 → Result<void> + caller 일괄 전환"
```

---

### Task 4: CoreEngine + cross_core — post_to/cross_core_post 반환 타입 전환

**Files:**
- Modify: `apex_core/include/apex/core/core_engine.hpp:99-101`
- Modify: `apex_core/src/core_engine.cpp:119-131`
- Modify: `apex_core/include/apex/core/cross_core_call.hpp:183-194`
- Modify: `apex_core/include/apex/core/server.hpp:138-142`
- Test: `apex_core/tests/unit/test_cross_core_call.cpp`
- Verify: `apex_core/tests/integration/test_pipeline_integration.cpp` (DispatchError 참조는 Task 3에서 수정 완료, cross_core 관련 검증만)

- [ ] **Step 1: test_cross_core_call.cpp 변경**

`apex_core/tests/unit/test_cross_core_call.cpp`:

**PostFireAndForget** (라인 80-89) — bool → Result 체크:
```cpp
TEST_F(CrossCoreCallTest, PostFireAndForget) {
    std::atomic<int> value{0};

    auto posted = server_->cross_core_post(1, [&value] {
        value.store(99, std::memory_order_relaxed);
    });
    EXPECT_TRUE(posted.has_value());

    ASSERT_TRUE(apex::test::wait_for([&] { return value.load() == 99; }));
}
```

**CrossCoreCallQueueFullTest** (라인 162-165) — cross_core_post bool → Result:
```cpp
    for (int i = 0; i < 2; ++i) {
        auto posted = server->cross_core_post(1, [sentinel] { /* no-op */ });
        ASSERT_TRUE(posted.has_value()) << "post #" << i << " should succeed";
    }
```

- [ ] **Step 2: test_pipeline_integration.cpp 검증**

`apex_core/tests/integration/test_pipeline_integration.cpp`:

> `UnknownMessageIdHandledGracefully`의 `DispatchError` 참조는 **Task 3에서 이미 수정됨**. `EncodeDecodeDispatch`/`MultiFramePipeline`은 `.has_value()`만 사용하므로 dispatch 반환 타입 변경에도 **코드 변경 불필요** (타입만 바뀌고 텍스트는 동일). `CoreEngineInterCoreDelivery` (라인 155-158)의 `(void)engine.post_to(...)` — `Result<void>`에도 `(void)` 캐스트 유효, **변경 불필요**.

- [ ] **Step 3: 테스트 실패 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug"`
Expected: FAIL — `cross_core_post` 반환 타입이 아직 `bool`

- [ ] **Step 4: core_engine.hpp/cpp 변경**

`apex_core/include/apex/core/core_engine.hpp` — include 추가 + post_to 반환 타입:

include 섹션에 추가:
```cpp
#include <apex/core/result.hpp>
```

post_to 선언 변경 (라인 99-101):
```cpp
    /// Post a message to a specific core's MPSC inbox. Thread-safe.
    /// @return ErrorCode::CrossCoreQueueFull if target core's queue is full.
    [[nodiscard]] Result<void> post_to(uint32_t target_core, CoreMessage msg);
```

`apex_core/src/core_engine.cpp` — post_to 구현 변경 (라인 119-125):
```cpp
Result<void> CoreEngine::post_to(uint32_t target_core, CoreMessage msg) {
    if (target_core >= cores_.size()) {
        return error(ErrorCode::Unknown);
    }
    auto result = cores_[target_core]->inbox->enqueue(msg);
    if (!result) {
        return error(ErrorCode::CrossCoreQueueFull);
    }
    return ok();
}
```

broadcast (라인 127-131) — `(void)` 캐스트로 Result 무시 (best-effort):
```cpp
void CoreEngine::broadcast(CoreMessage msg) {
    for (uint32_t i = 0; i < cores_.size(); ++i) {
        (void)post_to(i, msg);
    }
}
```

- [ ] **Step 5: cross_core_call.hpp 변경**

`apex_core/include/apex/core/cross_core_call.hpp`:

**cross_core_call (non-void, 라인 103-106)** — post_to 호출부:
```cpp
    auto post_result = engine.post_to(target_core, msg);
    if (!post_result) {
        delete task;
        co_return apex::core::error(post_result.error());
    }
```

**cross_core_call (void, 라인 163-166)** — 동일 패턴:
```cpp
    auto post_result = engine.post_to(target_core, msg);
    if (!post_result) {
        delete task;
        co_return apex::core::error(post_result.error());
    }
```

**cross_core_post (라인 183-194)** — bool → Result<void>:
```cpp
/// Fire-and-forget execution on target core (no timeout, no result).
/// Preferred over cross_core_call() for high-frequency inter-core messaging
/// where the caller does not need a response.
/// Returns ErrorCode::CrossCoreQueueFull if queue is full.
template <typename F>
Result<void> cross_core_post(CoreEngine& engine, uint32_t target_core, F&& func) {
    auto* task = new std::function<void()>(std::forward<F>(func));
    CoreMessage msg;
    msg.type = CoreMessage::Type::CrossCorePost;
    msg.data = reinterpret_cast<uint64_t>(task);
    auto result = engine.post_to(target_core, msg);
    if (!result) {
        delete task;
        return result;
    }
    return ok();
}
```

- [ ] **Step 6: server.hpp cross_core_post wrapper 변경**

`apex_core/include/apex/core/server.hpp` (라인 138-142):
```cpp
    /// Fire-and-forget execution on target core.
    template <typename F>
    Result<void> cross_core_post(uint32_t target_core, F&& func) {
        return apex::core::cross_core_post(
            *core_engine_, target_core, std::forward<F>(func));
    }
```

- [ ] **Step 7: 전체 빌드 + 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS — cross_core_call, pipeline_integration 테스트 포함 전부 통과

주의: `test_pipeline_integration.cpp`의 `CoreEngineInterCoreDelivery` (라인 155-158)에서 `(void)engine.post_to(...)` 사용 — `Result<void>`에도 `(void)` 캐스트가 유효하므로 변경 불필요.

- [ ] **Step 8: 커밋**

```bash
git add apex_core/include/apex/core/core_engine.hpp apex_core/src/core_engine.cpp \
        apex_core/include/apex/core/cross_core_call.hpp apex_core/include/apex/core/server.hpp \
        apex_core/tests/unit/test_cross_core_call.cpp \
        apex_core/tests/integration/test_pipeline_integration.cpp
git commit -m "refactor(core): post_to/cross_core_post → Result<void> + 계층별 에러 매핑"
```

---

## Chunk 2: Session async_send 전환 + Examples 정리 (Tasks 5–6)

### Task 5: Session async_send / async_send_raw → Result<void>

**Files:**
- Modify: `apex_core/include/apex/core/session.hpp:51-64`
- Modify: `apex_core/src/session.cpp:25-60`
- Test: `apex_core/tests/unit/test_session.cpp`

- [ ] **Step 1: test_session.cpp 변경 — bool → Result<void>**

`apex_core/tests/unit/test_session.cpp`:

include 추가:
```cpp
#include <apex/core/result.hpp>
```

**SendFrame** (라인 46-47):
```cpp
    auto result = run_coro(io_ctx_, session->async_send(header, payload));
    EXPECT_TRUE(result.has_value());
```

**SendAfterClose** (라인 70-71):
```cpp
    auto result = run_coro(io_ctx_, session->async_send(header, payload));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::core::ErrorCode::SessionClosed);
```

**SendRawSucceeds** (라인 109):
```cpp
    auto result = run_coro(io_ctx_, session->async_send_raw(raw_frame));
    EXPECT_TRUE(result.has_value());
```

**SendRawAfterCloseReturnsFalse** (라인 128):
```cpp
    auto result = run_coro(io_ctx_, session->async_send_raw(data));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::core::ErrorCode::SessionClosed);
```

**SendAfterPeerDisconnect_DoesNotCrash** (라인 167):
```cpp
    auto result = run_coro(io_ctx_, session->async_send(header, payload));
    // Send may succeed (buffered) or fail (connection reset).
    if (result.has_value()) {
        io_ctx_.run_for(std::chrono::milliseconds(10));
        io_ctx_.restart();
        auto result2 = run_coro(io_ctx_, session->async_send(header, payload));
        (void)result2;
    }
```

- [ ] **Step 2: 테스트 실패 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -R test_session -V`
Expected: FAIL — async_send 반환 타입이 아직 `bool`

- [ ] **Step 3: session.hpp — async_send/async_send_raw 선언 변경**

`apex_core/include/apex/core/session.hpp`:

include 추가 (기존 include 사이에):
```cpp
#include <apex/core/result.hpp>
```

선언 변경 (라인 59-64):
```cpp
    [[nodiscard]] boost::asio::awaitable<Result<void>>
    async_send(const WireHeader& header, std::span<const uint8_t> payload);

    /// 미리 빌드된 로우 프레임 비동기 전송.
    [[nodiscard]] boost::asio::awaitable<Result<void>>
    async_send_raw(std::span<const uint8_t> data);
```

- [ ] **Step 4: session.cpp — 구현 변경**

`apex_core/src/session.cpp`:

include 추가:
```cpp
#include <apex/core/result.hpp>
```

**async_send** (라인 25-46):
```cpp
awaitable<Result<void>> Session::async_send(const WireHeader& header,
                                            std::span<const uint8_t> payload) {
    if (!is_open()) co_return error(ErrorCode::SessionClosed);

    std::array<uint8_t, WireHeader::SIZE> hdr_buf{};
    header.serialize(hdr_buf);

    std::array<boost::asio::const_buffer, 2> buffers{
        boost::asio::buffer(hdr_buf),
        boost::asio::buffer(payload.data(), payload.size())
    };

    auto [ec, bytes] = co_await boost::asio::async_write(
        socket_, buffers,
        boost::asio::as_tuple(boost::asio::use_awaitable));

    if (ec) {
        close();
        co_return error(ErrorCode::SendFailed);
    }
    co_return ok();
}
```

**async_send_raw** (라인 48-60):
```cpp
awaitable<Result<void>> Session::async_send_raw(std::span<const uint8_t> data) {
    if (!is_open()) co_return error(ErrorCode::SessionClosed);

    auto [ec, bytes] = co_await boost::asio::async_write(
        socket_, boost::asio::buffer(data.data(), data.size()),
        boost::asio::as_tuple(boost::asio::use_awaitable));

    if (ec) {
        close();
        co_return error(ErrorCode::SendFailed);
    }
    co_return ok();
}
```

- [ ] **Step 5: 테스트 통과 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -R test_session -V`
Expected: PASS — 9개 테스트 전부 통과

- [ ] **Step 6: 커밋**

```bash
git add apex_core/include/apex/core/session.hpp apex_core/src/session.cpp \
        apex_core/tests/unit/test_session.cpp
git commit -m "refactor(session): async_send/async_send_raw → Result<void>"
```

---

### Task 6: Examples + server.cpp async_send callers + 전체 검증

**Files:**
- Verify: `apex_core/src/server.cpp` (Task 3에서 단순화된 async_send_raw 호출 — `(void)` 캐스트 유효, 변경 불필요)
- Verify: `apex_core/examples/echo_server.cpp:55` (`(void)` 캐스트 유효, 변경 불필요)
- Verify: `apex_core/examples/multicore_echo_server.cpp:55` (동일, 변경 불필요)
- Modify: `apex_core/examples/chat_server.cpp:62`
- Modify: `apex_core/tests/integration/test_server_e2e.cpp:99`

- [ ] **Step 1: server.cpp — async_send_raw 호출 검증**

Task 3에서 단순화된 server.cpp process_frames의 async_send_raw 호출:
```cpp
        if (!result.has_value()) {
            auto error_frame = ErrorSender::build_error_frame(
                header.msg_id, result.error());
            (void)co_await session->async_send_raw(error_frame);
        }
```
`(void)` 캐스트로 `Result<void>` 무시 — 의도적 (에러 응답 전송 실패는 무시 가능). **변경 불필요.**

- [ ] **Step 2: examples 변경**

`apex_core/examples/echo_server.cpp` (라인 55), `multicore_echo_server.cpp` (라인 55):
기존 `(void)co_await session->async_send(...)` — `(void)` 캐스트가 `Result<void>`에도 유효. **변경 불필요.**

`apex_core/examples/chat_server.cpp` (라인 62-65):

**Before:**
```cpp
            if (!co_await s->async_send(header, payload_span)) {
                // Send failed — peer likely disconnected. Session cleanup
                // is handled by the read_loop, so we just skip here.
            }
```

**After:**
```cpp
            if (!(co_await s->async_send(header, payload_span)).has_value()) {
                // Send failed — peer likely disconnected. Session cleanup
                // is handled by the read_loop, so we just skip here.
            }
```
> `std::expected`는 `explicit operator bool()`을 제공하므로 `!co_await ...` 패턴도 컴파일되지만, `Result<void>` 반환을 `.has_value()`로 명시하는 것이 `bool` 반환과 혼동 없이 의도가 명확해진다 (가독성/일관성 선택). 주석과 빈 블록은 원본 유지.

- [ ] **Step 3: test_server_e2e.cpp — on_echo 내 async_send `[[nodiscard]]` 대응 (필수)**

`apex_core/tests/integration/test_server_e2e.cpp` (라인 99):

**Before:**
```cpp
        co_await session->async_send(header, {builder.GetBufferPointer(), builder.GetSize()});
```

**After:**
```cpp
        (void)co_await session->async_send(header, {builder.GetBufferPointer(), builder.GetSize()});
```
> `[[nodiscard]]` 반환값 무시 시 `(void)` 캐스트 **필수**. GCC에서 `-Werror` 환경이므로 누락 시 빌드 실패.

- [ ] **Step 4: 전체 빌드 + 전체 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS — 전 테스트 통과

- [ ] **Step 5: ASAN 빌드 테스트** (메모리 안전성 확인)

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat asan" && ctest --preset asan -V`
Expected: PASS — 메모리 누수/접근 위반 없음

- [ ] **Step 6: 커밋**

```bash
git add apex_core/examples/chat_server.cpp apex_core/tests/integration/test_server_e2e.cpp
git commit -m "refactor(examples): async_send Result<void> 전환 적용"
```

---

## Summary

### 제거된 타입 (2개)
- `DispatchError` enum (message_dispatcher.hpp)
- `QueueError` enum (mpsc_queue.hpp)

### 추가된 ErrorCode (2개)
- `HandlerException = 11` — handler 예외 (구 DispatchError::HandlerFailed)
- `SendFailed = 12` — async_send 네트워크 에러

### 통일된 반환 타입 (6개 함수)
| 함수 | Before | After |
|------|--------|-------|
| `MpscQueue::enqueue()` | `expected<void, QueueError>` | `Result<void>` (BufferFull) |
| `CoreEngine::post_to()` | `bool` | `Result<void>` (CrossCoreQueueFull) |
| `cross_core_post()` | `bool` | `Result<void>` (CrossCoreQueueFull) |
| `MessageDispatcher::dispatch()` | `awaitable<expected<Result<void>, DispatchError>>` | `awaitable<Result<void>>` |
| `Session::async_send()` | `awaitable<bool>` | `awaitable<Result<void>>` (SessionClosed/SendFailed) |
| `Session::async_send_raw()` | `awaitable<bool>` | `awaitable<Result<void>>` (SessionClosed/SendFailed) |

### 계층별 에러 매핑
```
MpscQueue::enqueue → ErrorCode::BufferFull (범용)
    └→ CoreEngine::post_to → ErrorCode::CrossCoreQueueFull (크로스코어 의미 부여)
        └→ cross_core_call/post → propagate as-is
```

### 수정 파일 수: 23개
- 헤더: 7개 (error_code, mpsc_queue, message_dispatcher, core_engine, cross_core_call, session, server)
- 소스: 4개 (message_dispatcher, core_engine, session, server)
- 테스트: 9개 (신규 1 + 수정 8: error_propagation, mpsc_queue, message_dispatcher, service_base, flatbuffers_dispatch, session, cross_core_call, pipeline_integration, server_e2e)
- 예제: 3개 (실제 변경 1개 — chat_server. echo/multicore_echo는 `(void)` 캐스트 유효, 검증만)

### Task 별 커밋 (6개)
1. `feat(error): ErrorCode에 HandlerException, SendFailed 추가`
2. `refactor(mpsc): QueueError 제거 → Result<void> + ErrorCode::BufferFull`
3. `refactor(dispatch): DispatchError 제거 → Result<void> + caller 일괄 전환` ← server.cpp + test_service_base.cpp + test_flatbuffers_dispatch.cpp + test_pipeline_integration.cpp + test_server_e2e.cpp 포함
4. `refactor(core): post_to/cross_core_post → Result<void> + 계층별 에러 매핑`
5. `refactor(session): async_send/async_send_raw → Result<void>`
6. `refactor(examples): async_send Result<void> 전환 적용`
