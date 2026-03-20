# Backlog Sweep Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** IN VIEW 백로그 15건을 일괄 수정하여 코드 위생, 설계 부채, 문서 정합성을 개선한다.

**Architecture:** 코드 수정(Task 1-9)을 먼저 진행하고 문서 갱신(Task 10)은 마지막에 일괄 처리. 기존 71개 유닛 테스트 + 11개 E2E가 모든 변경 후에도 통과해야 한다. 각 Task는 독립적이며 순서 변경 가능하나, Task 8(ServerConfig 분리)은 가장 넓은 include 영향을 가지므로 다른 코드 수정 후 진행 권장.

**Tech Stack:** C++23, Boost.Asio, MSVC 19.44, CMake + Ninja, vcpkg

**해결 백로그:** #2, #10, #11, #68, #70, #71, #72, #92, #93, #94, #95, #96, #69, #97②, #97④

---

## File Structure

### 신규 생성
| 파일 | 역할 |
|------|------|
| `apex_core/include/apex/core/server_config.hpp` | ServerConfig 경량 헤더 (Task 8) |

### 주요 수정
| 파일 | Task | 변경 내용 |
|------|------|-----------|
| `apex_core/include/apex/core/service_base.hpp:306` | 1 | fetch_add 메모리 오더 |
| `apex_core/include/apex/core/connection_handler.hpp:169` | 2 | WebSocket msg_id 바이트오더 |
| `apex_services/chat-svc/src/chat_service.cpp:34-45` | 3 | safe_parse_u64 → Result |
| `apex_services/gateway/include/apex/gateway/gateway_service.hpp` | 4,6 | unordered_map 교체 + 멤버 함수 선언 |
| `apex_services/gateway/include/apex/gateway/channel_session_map.hpp` | 4 | unordered_map → flat_map |
| `apex_services/gateway/include/apex/gateway/pending_requests.hpp` | 4 | unordered_map → flat_map |
| `apex_services/gateway/src/gateway_service.cpp:110-197` | 6 | 람다 → 멤버 함수 3개 추출 |
| `apex_services/chat-svc/include/apex/chat_svc/chat_service.hpp` | 5 | 채널 상수 정의 |
| `apex_services/chat-svc/src/chat_service.cpp:686` | 5 | 상수 사용 |
| `apex_shared/lib/adapters/common/include/apex/shared/adapters/circuit_breaker.hpp` | 7 | Result<T> 제네릭화 + HALF_OPEN 원자적 |
| `apex_shared/lib/adapters/common/src/circuit_breaker.cpp:26-47` | 7 | should_allow() 원자적 카운터 |
| `apex_core/include/apex/core/config.hpp` | 8 | server.hpp → server_config.hpp 교체 |
| `apex_core/include/apex/core/server.hpp` | 8 | ServerConfig 분리 후 include |
| `apex_shared/lib/adapters/redis/src/redis_multiplexer.cpp:366-392` | 9 | cancel_all_pending UAF 수정 |
| `apex_shared/lib/adapters/redis/include/apex/shared/adapters/redis/redis_multiplexer.hpp:120` | 9 | PendingCommand에 cancelled 플래그 |
| `docs/apex_core/apex_core_guide.md` | 10 | §3 Shutdown + §4.1 Registry + §8 mutex 예외 |
| `docs/Apex_Pipeline.md` | 10 | post_init_callback 기재 수정 |
| `apex_core/README.md` | 10 | WireHeader 크기, 의존성, 프로토콜 위치 갱신 |

---

## Task 1: #94 outstanding_coros_ 메모리 오더 정합

**Files:**
- Modify: `apex_core/include/apex/core/service_base.hpp:306`

**배경:** `spawn()`에서 `fetch_add(1, relaxed)` 사용하지만 shutdown에서 `load(acquire)`와 쌍이 맞지 않음. `fetch_sub(1, release)`와 대칭이 되려면 `fetch_add`도 `acq_rel`이어야 안전.

- [ ] **Step 1: 메모리 오더 수정**

`service_base.hpp:306`에서:
```cpp
// Before:
outstanding_coros_.fetch_add(1, std::memory_order_relaxed);
// After:
outstanding_coros_.fetch_add(1, std::memory_order_acq_rel);
```

- [ ] **Step 2: 빌드 검증**

```bash
"<PROJECT_ROOT>/apex_tools/queue-lock.sh" build debug
```
기존 71개 유닛 테스트 통과 확인.

- [ ] **Step 3: 커밋**

```bash
git add apex_core/include/apex/core/service_base.hpp
git commit -m "fix(core): BACKLOG-94 outstanding_coros_ fetch_add 메모리 오더 acq_rel 정합"
```

---

## Task 2: #92 WebSocket msg_id 바이트오더 명시

**Files:**
- Modify: `apex_core/include/apex/core/connection_handler.hpp:160-171`

**배경:** TCP(WireHeader)는 big-endian, WebSocket은 `memcpy`로 host byte order 사용. 프로토콜 간 바이트오더 불일치. WebSocket은 클라이언트와 호스트가 동일 바이트오더를 전제하므로, 명시적 big-endian 변환(`ntohl`)으로 TCP와 통일.

- [ ] **Step 1: ntohl 변환 추가**

`connection_handler.hpp:169` 부근 수정:
```cpp
// Before:
std::memcpy(&msg_id, raw.data(), sizeof(uint32_t));

// After:
uint32_t raw_id = 0;
std::memcpy(&raw_id, raw.data(), sizeof(uint32_t));
msg_id = ntohl(raw_id);  // big-endian → host (TCP WireHeader와 통일)
```

플랫폼 헤더 include 필요 — 파일 상단에 이미 있는지 확인. 없으면 추가:
```cpp
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
```
단, `connection_handler.hpp`는 Boost.Asio를 이미 include하므로 `<boost/asio/detail/socket_ops.hpp>`를 통해 사용 가능할 수 있다. `winsock2.h`가 이미 간접 include되는지 확인 후 판단.

- [ ] **Step 2: 빌드 검증**

빌드 + 테스트 통과 확인.

- [ ] **Step 3: 커밋**

```bash
git add apex_core/include/apex/core/connection_handler.hpp
git commit -m "fix(core): BACKLOG-92 WebSocket msg_id big-endian 변환 — TCP WireHeader와 바이트오더 통일"
```

---

## Task 3: #72 safe_parse_u64 → Result<uint64_t>

**Files:**
- Modify: `apex_services/chat-svc/src/chat_service.cpp:34-45` (함수 정의)
- Modify: `apex_services/chat-svc/src/chat_service.cpp` (호출부 10곳)

**배경:** 실패 시 0 반환하지만 room_id, user_id 등에서 0이 유효값일 수 있어 조용한 실패 가능.

- [ ] **Step 1: 함수 시그니처 변경**

```cpp
// Before (line 34-45):
uint64_t safe_parse_u64(std::string_view sv, std::string_view context = "") noexcept
{
    uint64_t value = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec != std::errc{})
    {
        spdlog::warn("[ChatService] Failed to parse uint64 '{}' (context: {})", sv, context);
        return 0;
    }
    return value;
}

// After:
apex::core::Result<uint64_t> safe_parse_u64(std::string_view sv, std::string_view context = "") noexcept
{
    uint64_t value = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec != std::errc{})
    {
        spdlog::warn("[ChatService] Failed to parse uint64 '{}' (context: {})", sv, context);
        return std::unexpected(apex::core::ErrorCode::InvalidMessage);
    }
    return value;
}
```

파일 상단에 `#include <apex/core/result.hpp>` 추가 필요 (이미 있을 수 있으니 확인).

- [ ] **Step 2: 호출부 10곳 수정**

모든 호출부를 다음 패턴으로 수정:
```cpp
// Before:
auto room_id = safe_parse_u64(pg_result->value(0, 0), "create_room.room_id");

// After:
auto room_id_result = safe_parse_u64(pg_result->value(0, 0), "create_room.room_id");
if (!room_id_result.has_value())
    co_return std::unexpected(room_id_result.error());
auto room_id = *room_id_result;
```

호출부 위치 (chat_service.cpp):
- line 209: `create_room.room_id`
- line 264: `join_room.max_members` (uint32_t cast)
- line 355: `list_rooms.total_count` (uint32_t cast)
- line 382: `list_rooms.room_id` (루프 내)
- line 384: `list_rooms.max_members` (루프 내, uint32_t cast)
- line 385: `list_rooms.owner_id` (루프 내)
- line 541: `whisper.target_session_id`
- line 642: `history.message_id` (루프 내)
- line 643: `history.sender_id` (루프 내)
- line 646: `history.timestamp` (루프 내)

루프 내 호출(382, 384, 385, 642, 643, 646)은 `continue`로 처리:
```cpp
auto rid_result = safe_parse_u64(pg_res.value(i, 0), "list_rooms.room_id");
if (!rid_result.has_value())
    continue;
auto rid = *rid_result;
```

- [ ] **Step 3: 빌드 검증**

- [ ] **Step 4: 커밋**

```bash
git add apex_services/chat-svc/src/chat_service.cpp
git commit -m "fix(chat-svc): BACKLOG-72 safe_parse_u64 Result<uint64_t> 반환 — 0 반환 silent failure 해소"
```

---

## Task 4: #97② unordered_map → boost::unordered_flat_map

**Files:**
- Modify: `apex_services/gateway/include/apex/gateway/gateway_service.hpp:17,149`
- Modify: `apex_services/gateway/include/apex/gateway/channel_session_map.hpp:8,50,52`
- Modify: `apex_services/gateway/include/apex/gateway/pending_requests.hpp:10,58`

**배경:** 코어/shared는 이미 `boost::unordered_flat_map` 사용 (8곳). Gateway만 `std::unordered_map` 잔존.

- [ ] **Step 1: gateway_service.hpp 수정**

```cpp
// line 17: #include <unordered_map> → 제거
// 추가: #include <boost/unordered/unordered_flat_map.hpp>

// line 149:
// Before:
std::unordered_map<apex::core::SessionId, AuthState> auth_states_;
// After:
boost::unordered_flat_map<apex::core::SessionId, AuthState> auth_states_;
```

- [ ] **Step 2: channel_session_map.hpp 수정**

```cpp
// line 8: #include <unordered_map> → 제거 (unordered_set은 유지 — session_to_channels_ 값 타입)
// 추가: #include <boost/unordered/unordered_flat_map.hpp>

// line 50:
// Before:
std::unordered_map<std::string, std::vector<apex::core::SessionId>> channel_to_sessions_;
// After:
boost::unordered_flat_map<std::string, std::vector<apex::core::SessionId>> channel_to_sessions_;

// line 52:
// Before:
std::unordered_map<apex::core::SessionId, std::unordered_set<std::string>> session_to_channels_;
// After:
boost::unordered_flat_map<apex::core::SessionId, std::unordered_set<std::string>> session_to_channels_;
```

- [ ] **Step 3: pending_requests.hpp 수정**

```cpp
// line 10: #include <unordered_map> → 제거
// 추가: #include <boost/unordered/unordered_flat_map.hpp>

// line 58:
// Before:
std::unordered_map<uint64_t, PendingEntry> map_;
// After:
boost::unordered_flat_map<uint64_t, PendingEntry> map_;
```

**주의:** `boost::unordered_flat_map`은 값 타입이 move-only여도 동작하지만, `PendingEntry`에 `steady_timer` 등이 있으면 확인 필요.

- [ ] **Step 4: 빌드 검증**

- [ ] **Step 5: 커밋**

```bash
git add apex_services/gateway/include/apex/gateway/gateway_service.hpp \
       apex_services/gateway/include/apex/gateway/channel_session_map.hpp \
       apex_services/gateway/include/apex/gateway/pending_requests.hpp
git commit -m "refactor(gateway): BACKLOG-97 std::unordered_map → boost::unordered_flat_map 4곳 교체"
```

---

## Task 5: #97④ "pub:global:chat" 문자열 상수화

**Files:**
- Modify: `apex_services/chat-svc/include/apex/chat_svc/chat_service.hpp:55`
- Modify: `apex_services/chat-svc/src/chat_service.cpp:686`

**배경:** `"pub:global:chat"` 문자열이 소스 5+곳에 산재. TOML/테스트/스키마의 문자열은 각자 역할이 있으므로 유지하되, 코드 내 사용은 상수로 통일.

- [ ] **Step 1: 상수 정의**

`chat_service.hpp`에 상수 추가 (ChatService 클래스 위 또는 namespace 수준):
```cpp
/// Global broadcast channel name (Redis Pub/Sub).
inline constexpr std::string_view GLOBAL_CHAT_CHANNEL = "pub:global:chat";
```

- [ ] **Step 2: 코드 내 사용처 교체**

`chat_service.cpp:686`:
```cpp
// Before:
auto global_channel = std::string("pub:global:chat");
// After:
auto global_channel = std::string(GLOBAL_CHAT_CHANNEL);
```

- [ ] **Step 3: 빌드 검증**

- [ ] **Step 4: 커밋**

```bash
git add apex_services/chat-svc/include/apex/chat_svc/chat_service.hpp \
       apex_services/chat-svc/src/chat_service.cpp
git commit -m "refactor(chat-svc): BACKLOG-97 pub:global:chat 문자열 상수화"
```

---

## Task 6: #68 + #97⑧ Gateway default handler 리팩터링

**Files:**
- Modify: `apex_services/gateway/include/apex/gateway/gateway_service.hpp` (멤버 함수 선언 추가)
- Modify: `apex_services/gateway/src/gateway_service.cpp:110-197` (람다 → 멤버 함수)

**배경:** `on_start()`의 83줄 인라인 람다를 3개 멤버 함수로 추출하고, `set_default_handler(member_function_pointer)` ServiceBase 패턴 적용. `dispatcher().set_default_handler(lambda)` 우회 해소.

- [ ] **Step 1: 헤더에 멤버 함수 선언 추가**

`gateway_service.hpp` private 섹션에 추가:
```cpp
    /// System message: JWT 바인딩 (AUTHENTICATE_SESSION).
    apex::core::Result<void> handle_authenticate_session(apex::core::SessionPtr session,
                                                          std::span<const uint8_t> payload);

    /// System message: 채널 구독 (SUBSCRIBE_CHANNEL).
    apex::core::Result<void> handle_subscribe_channel(apex::core::SessionPtr session,
                                                       std::span<const uint8_t> payload);

    /// System message: 채널 구독 해제 (UNSUBSCRIBE_CHANNEL).
    apex::core::Result<void> handle_unsubscribe_channel(apex::core::SessionPtr session,
                                                         std::span<const uint8_t> payload);

    /// Default handler: 시스템 메시지 분기 + 서비스 메시지 라우팅.
    boost::asio::awaitable<apex::core::Result<void>> on_default_message(
        apex::core::SessionPtr session, uint32_t msg_id, std::span<const uint8_t> payload);
```

- [ ] **Step 2: 멤버 함수 구현 + on_start() 리팩터링**

`gateway_service.cpp`에서 `on_start()` 전체를 다음으로 교체:

```cpp
void GatewayService::on_start()
{
    set_default_handler(&GatewayService::on_default_message);
}
```

`on_default_message` 구현:
```cpp
boost::asio::awaitable<apex::core::Result<void>> GatewayService::on_default_message(
    apex::core::SessionPtr session, uint32_t msg_id, std::span<const uint8_t> payload)
{
    if (msg_id == system_msg_ids::AUTHENTICATE_SESSION)
        co_return handle_authenticate_session(std::move(session), payload);

    if (msg_id == system_msg_ids::SUBSCRIBE_CHANNEL)
        co_return handle_subscribe_channel(std::move(session), payload);

    if (msg_id == system_msg_ids::UNSUBSCRIBE_CHANNEL)
        co_return handle_unsubscribe_channel(std::move(session), payload);

    co_return co_await handle_request(std::move(session), msg_id, payload);
}
```

그리고 기존 람다의 3개 분기를 각각 `handle_authenticate_session`, `handle_subscribe_channel`, `handle_unsubscribe_channel`로 추출. 로직은 그대로 유지.

**주의:** `set_default_handler(member_function_pointer)` 패턴의 시그니처가 `on_default_message`와 일치하는지 확인 필요. ServiceBase의 `set_default_handler`는 `(SessionPtr, uint32_t, span<const uint8_t>) -> awaitable<Result<void>>` 시그니처의 멤버 함수 포인터를 받음.

- [ ] **Step 3: 빌드 검증**

- [ ] **Step 4: 커밋**

```bash
git add apex_services/gateway/include/apex/gateway/gateway_service.hpp \
       apex_services/gateway/src/gateway_service.cpp
git commit -m "refactor(gateway): BACKLOG-68,97 default handler 람다 → 멤버 함수 3개 추출"
```

---

## Task 7: #10 + #11 CircuitBreaker 수정

**Files:**
- Modify: `apex_shared/lib/adapters/common/include/apex/shared/adapters/circuit_breaker.hpp`
- Modify: `apex_shared/lib/adapters/common/src/circuit_breaker.cpp:26-47`
- Modify: `apex_shared/tests/test_circuit_breaker.cpp` (테스트 추가)

### Part A: #11 Result<T> 제네릭화

- [ ] **Step 1: awaitable Result concept 정의 + 템플릿 시그니처 변경**

`circuit_breaker.hpp` 상단(namespace 내)에 concept 추가:
```cpp
/// CircuitBreaker::call()이 받을 수 있는 callable의 반환 타입 제약.
/// F()는 반드시 awaitable<Result<T>>를 반환해야 한다.
template <typename F>
concept CircuitCallable = std::invocable<F> && requires {
    // invoke_result_t가 awaitable<Result<T>> 형태인지 확인
    typename std::invoke_result_t<F>::value_type;  // awaitable의 value_type = Result<T>
};
```

선언부 변경:
```cpp
// Before (line 33-36):
template <std::invocable F>
    requires std::same_as<std::invoke_result_t<F>, boost::asio::awaitable<apex::core::Result<void>>>
[[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> call(F&& fn);

// After:
template <CircuitCallable F>
[[nodiscard]] std::invoke_result_t<F> call(F&& fn);
```

구현부 (line 54-74):
```cpp
// Before:
template <std::invocable F>
    requires std::same_as<std::invoke_result_t<F>, boost::asio::awaitable<apex::core::Result<void>>>
[[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> CircuitBreaker::call(F&& fn)
{
    if (!should_allow())
    {
        co_return std::unexpected(apex::core::ErrorCode::CircuitOpen);
    }
    auto result = co_await std::forward<F>(fn)();
    if (result.has_value())
        on_success();
    else
        on_failure();
    co_return result;
}

// After:
template <CircuitCallable F>
[[nodiscard]] std::invoke_result_t<F> CircuitBreaker::call(F&& fn)
{
    if (!should_allow())
    {
        co_return std::unexpected(apex::core::ErrorCode::CircuitOpen);
    }
    auto result = co_await std::forward<F>(fn)();
    if (result.has_value())
        on_success();
    else
        on_failure();
    co_return result;
}
```

이렇게 하면 `F`가 `awaitable<Result<T>>`를 반환하는 callable만 허용되고, `Result<void>` 뿐 아니라 `Result<int>` 등도 사용 가능.

### Part B: #10 HALF_OPEN should_allow() 원자적 카운터

- [ ] **Step 2: should_allow에 원자적 카운터 도입**

CircuitBreaker는 per-core 어댑터에서 사용되므로 단일 스레드 보장이 되지만, 코루틴 인터리빙 문제가 있음. `should_allow()` + `on_success()`/`on_failure()` 사이에 다른 코루틴이 끼어들 수 있음.

**핵심 문제:** OPEN→HALF_OPEN 전환은 `should_allow()`의 OPEN 분기에서 발생하고 `true`를 반환함. 이 첫 번째 호출도 카운팅해야 하지만 HALF_OPEN 분기를 거치지 않음. 따라서 OPEN 분기에서 전환 시 카운터를 1로 초기화하고, HALF_OPEN 분기에서도 진입 시 즉시 카운팅.

`circuit_breaker.cpp:26-47`에서:
```cpp
// Before:
case CircuitState::OPEN:
{
    auto elapsed = std::chrono::steady_clock::now() - open_since_;
    if (elapsed >= config_.open_duration)
    {
        state_ = CircuitState::HALF_OPEN;
        half_open_calls_ = 0;
        return true;
    }
    return false;
}
case CircuitState::HALF_OPEN:
    return half_open_calls_ < config_.half_open_max_calls;

// After:
case CircuitState::OPEN:
{
    auto elapsed = std::chrono::steady_clock::now() - open_since_;
    if (elapsed >= config_.open_duration)
    {
        state_ = CircuitState::HALF_OPEN;
        half_open_calls_ = 1;  // 이 호출 자체를 첫 번째로 카운팅
        return true;
    }
    return false;
}
case CircuitState::HALF_OPEN:
    if (half_open_calls_ >= config_.half_open_max_calls)
        return false;
    ++half_open_calls_;  // 진입 시점에 즉시 카운팅 (코루틴 인터리빙 방어)
    return true;
```

그리고 `on_success()`에서 중복 카운팅 제거:
```cpp
// Before:
case CircuitState::HALF_OPEN:
    ++half_open_calls_;
    if (half_open_calls_ >= config_.half_open_max_calls)
    {
        state_ = CircuitState::CLOSED;
        failure_count_ = 0;
    }
    break;

// After:
case CircuitState::HALF_OPEN:
    // half_open_calls_는 should_allow()에서 이미 증가됨 (OPEN→HALF_OPEN 전환 포함)
    if (half_open_calls_ >= config_.half_open_max_calls)
    {
        state_ = CircuitState::CLOSED;
        failure_count_ = 0;
    }
    break;
```

**카운팅 흐름 검증 (half_open_max_calls=2):**
1. OPEN→HALF_OPEN 전환: `half_open_calls_=1`, return true → 코루틴 실행 → on_success (1 < 2, 유지)
2. 다음 호출: HALF_OPEN 분기, `++half_open_calls_` → 2, return true → 코루틴 실행 → on_success (2 >= 2, CLOSED 전환)
3. 총 2회 성공 호출로 CLOSED 전환 — `half_open_max_calls=2`와 일치 ✅

- [ ] **Step 3: 빌드 검증**

- [ ] **Step 4: 커밋**

```bash
git add apex_shared/lib/adapters/common/include/apex/shared/adapters/circuit_breaker.hpp \
       apex_shared/lib/adapters/common/src/circuit_breaker.cpp
git commit -m "fix(shared): BACKLOG-10,11 CircuitBreaker HALF_OPEN 인터리빙 수정 + Result<T> 제네릭화"
```

---

## Task 8: #93 config.hpp → server_config.hpp 분리

**Files:**
- Create: `apex_core/include/apex/core/server_config.hpp`
- Modify: `apex_core/include/apex/core/config.hpp:7`
- Modify: `apex_core/include/apex/core/server.hpp` (ServerConfig 정의 → include로 교체)

**배경:** `config.hpp`가 `server.hpp`를 include하여 ~15개 헤더 체인 끌어옴. `ServerConfig`를 경량 헤더로 분리하면 컴파일 의존성 대폭 감소. TODO(I-01) 마커 존재.

- [ ] **Step 1: server_config.hpp 생성**

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace apex::core
{

/// Server configuration. Fields are ordered for designated-initializer convenience.
struct ServerConfig
{
    // Network (bind_address deferred — TcpAcceptor defaults to 0.0.0.0)
    // port 제거 — listen<P>(port, config)으로 대체
    bool tcp_nodelay = true; // Disable Nagle's algorithm for low-latency

    // Multicore
    uint32_t num_cores = 1;
    size_t mpsc_queue_capacity = 65536;
    std::chrono::milliseconds tick_interval{100};

    // Session
    uint32_t heartbeat_timeout_ticks = 300; // 0 = disabled
    size_t recv_buf_capacity = 8192;
    size_t timer_wheel_slots = 1024;

    // Platform I/O
    bool reuseport = false; // Linux: per-core SO_REUSEPORT, Windows: ignored

    // Lifecycle
    bool handle_signals = true;
    std::chrono::seconds drain_timeout{25}; // Graceful Shutdown drain timeout

    // Cross-core call
    std::chrono::milliseconds cross_core_call_timeout{5000};

    // Memory allocators (per-core)
    std::size_t bump_capacity_bytes = 64 * 1024; // 64KB
    std::size_t arena_block_bytes = 4096;        // 4KB
    std::size_t arena_max_bytes = 1024 * 1024;   // 1MB
};

} // namespace apex::core
```

- [ ] **Step 2: server.hpp에서 ServerConfig 제거 + include 추가**

`server.hpp` 상단에 `#include <apex/core/server_config.hpp>` 추가하고, ServerConfig struct 정의를 삭제.

- [ ] **Step 3: config.hpp에서 server.hpp → server_config.hpp 교체**

```cpp
// Before:
// TODO(I-01): config.hpp includes server.hpp solely for the ServerConfig definition.
// ...
#include <apex/core/server.hpp>

// After:
#include <apex/core/server_config.hpp>
```

TODO(I-01) 주석 제거.

- [ ] **Step 4: 빌드 검증**

이 변경은 include 체인에 영향이 넓으므로 빌드 실패 가능. 실패 시 누락된 include를 개별 파일에 추가.

- [ ] **Step 5: 커밋**

```bash
git add apex_core/include/apex/core/server_config.hpp \
       apex_core/include/apex/core/server.hpp \
       apex_core/include/apex/core/config.hpp
git commit -m "refactor(core): BACKLOG-93 ServerConfig → server_config.hpp 분리 — config.hpp 순환 include 해소"
```

---

## Task 9: #2 RedisMultiplexer cancel_all_pending UAF 수정

**Files:**
- Modify: `apex_shared/lib/adapters/redis/include/apex/shared/adapters/redis/redis_multiplexer.hpp:114-121`
- Modify: `apex_shared/lib/adapters/redis/src/redis_multiplexer.cpp:366-392`
- Modify: `apex_shared/lib/adapters/redis/src/redis_multiplexer.cpp:196-228` (static_on_reply)

**배경:** `cancel_all_pending()`에서 `timed_out` 경로와 `static_on_reply()` 경로 간 이중 `slab_.destroy()` 가능성. `cancel_all_pending()`이 명시적으로 소유권을 넘겨받는 `cancelled` 플래그 도입.

- [ ] **Step 1: PendingCommand에 cancelled 플래그 추가**

`redis_multiplexer.hpp:114-121`:
```cpp
struct PendingCommand
{
    boost::asio::steady_timer resolver;
    boost::asio::steady_timer timeout;
    apex::core::Result<RedisReply> result;
    uint64_t sequence;
    bool timed_out{false};
    bool cancelled{false};  // cancel_all_pending에서 소유권 이전 완료 플래그
};
```

- [ ] **Step 2: cancel_all_pending() 수정**

`redis_multiplexer.cpp:366-392`:
```cpp
void RedisMultiplexer::cancel_all_pending(apex::core::ErrorCode error)
{
    auto local = std::move(pending_);
    for (auto* cmd : local)
    {
        cmd->result = std::unexpected(error);
        cmd->cancelled = true;  // 소유권 표시

        if (cmd->timed_out)
        {
            // 이미 타임아웃됨 — 코루틴은 재개 완료. hiredis는 disconnect 후 콜백 안 함.
            // cancel_all_pending이 유일한 소유자이므로 안전하게 해제.
            slab_.destroy(cmd);
        }
        else
        {
            // 코루틴이 resolver.async_wait에 대기 중.
            // cancel()이 completion handler를 post → 코루틴 재개 → release_pending에서 해제.
            cmd->resolver.cancel();
        }
    }
}
```

- [ ] **Step 3: static_on_reply()에 cancelled 체크 추가**

`redis_multiplexer.cpp:196-228`:
```cpp
void RedisMultiplexer::static_on_reply(redisAsyncContext* /*ac*/, void* reply, void* privdata)
{
    auto* self = static_cast<RedisMultiplexer*>(privdata);
    auto* r = static_cast<redisReply*>(reply);

    if (self->pending_.empty())
        return;

    auto* front = self->pending_.front();
    self->pending_.pop_front();

    // cancel_all_pending가 이미 소유권을 가져간 경우 — 이중 해제 방지
    if (front->cancelled)
    {
        // cancel_all_pending이 timed_out 경로로 이미 destroy했거나
        // resolver.cancel() 경로로 코루틴이 해제할 예정. 여기서는 무시.
        return;
    }

    if (front->timed_out)
    {
        self->slab_.destroy(front);
        return;
    }

    if (r && r->type != REDIS_REPLY_ERROR)
    {
        front->result = RedisReply{r};
    }
    else
    {
        front->result = std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    front->resolver.cancel();
}
```

**핵심:** `cancel_all_pending()`이 `pending_`을 먼저 move-out하므로 `static_on_reply`의 `pending_.empty()` 체크로 대부분 걸리지만, move와 콜백 사이 타이밍에 따라 빠져나갈 수 있음. `cancelled` 플래그가 이중 안전장치.

- [ ] **Step 4: 빌드 검증**

- [ ] **Step 5: 커밋**

```bash
git add apex_shared/lib/adapters/redis/include/apex/shared/adapters/redis/redis_multiplexer.hpp \
       apex_shared/lib/adapters/redis/src/redis_multiplexer.cpp
git commit -m "fix(shared): BACKLOG-2 RedisMultiplexer cancel_all_pending UAF 수정 — cancelled 플래그 이중 해제 방지"
```

---

## Task 10: 문서 갱신 (#69, #70, #71, #96, #95)

**Files:**
- Modify: `docs/apex_core/apex_core_guide.md` (§3 Shutdown, §4.1 Registry, §8 mutex 예외)
- Modify: `docs/Apex_Pipeline.md` (post_init_callback 기재)
- Modify: `apex_core/README.md` (WireHeader, 의존성, 프로토콜)

### Part A: #70 ServiceRegistry API 예시 수정

- [ ] **Step 1:** `apex_core_guide.md`에서 `ctx.local_registry.get<T>()` 포인터 반환 예시를 `find<T>()` 사용으로 변경. `get<T>()`는 참조 반환 + 미등록 시 예외임을 명시.

### Part B: #71 PubSubListener mutex 예외 명시

- [ ] **Step 2:** `apex_core_guide.md` §8 #2 "모든 뮤텍스 금지" 원칙에 예외 조항 추가:

> **예외**: 외부 라이브러리가 전용 스레드를 운영하는 경우(예: hiredis 전용 Redis 스레드), 해당 스레드와 서비스 스레드 간 공유 데이터에 한해 `std::mutex` 허용. 현재 `PubSubListener`의 `channels_mutex_`가 이에 해당.

### Part C: #69 Shutdown 시퀀스 갱신

- [ ] **Step 3:** `apex_core_guide.md` §3 Shutdown 섹션을 `server.cpp:293-419` `finalize_shutdown()` 실제 구현 기준으로 갱신. 7단계 시퀀스 반영:
1. Listener stop (acceptor 종료)
2. Adapter drain
3. Scheduler stop_all
4. Service stop (on_stop 호출)
5. Outstanding coroutines drain wait [D7]
6. CoreEngine stop/join/drain_remaining
7. Adapter close + globals clear

### Part D: #96 post_init_callback 문서 수정

- [ ] **Step 4:** `docs/Apex_Pipeline.md`에서 "post_init_callback 완전 제거" 기재를 수정. 실제 코드에 `set_post_init_callback` API가 존재하고 3개 서비스 main.cpp에서 사용 중이므로, 해당 기록을 "post_init_callback 유지 (서비스 main.cpp에서 사용)" 등으로 정정.

### Part E: #95 apex_core/README.md 갱신

- [ ] **Step 5:** `apex_core/README.md` 갱신:
- WireHeader 크기: 10바이트 → 12바이트 (v2)
- "향후 추가 예정" 항목 중 이미 추가된 것 반영 (boost-beast, jwt-cpp 등)
- TcpBinaryProtocol 위치: core → shared 이동 반영
- 누락 컴포넌트 추가: BumpAllocator, ArenaAllocator

- [ ] **Step 6: 커밋**

```bash
git add docs/apex_core/apex_core_guide.md docs/Apex_Pipeline.md apex_core/README.md
git commit -m "docs: BACKLOG-69,70,71,95,96 프레임워크 가이드 + README + 설계문서 정합성 갱신"
```

---

## Task 11: 마무리 — clang-format + 빌드 + 백로그 갱신

- [ ] **Step 1: clang-format 전체 실행**

```bash
find apex_core apex_shared apex_services \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) ! -name '*_generated.h' | xargs clang-format -i
```

- [ ] **Step 2: 최종 빌드**

```bash
"<PROJECT_ROOT>/apex_tools/queue-lock.sh" build debug
```

71개 유닛 + E2E 통과 확인.

- [ ] **Step 3: BACKLOG.md 갱신**

완료된 15건을 `docs/BACKLOG.md`에서 삭제 → `docs/BACKLOG_HISTORY.md`에 이전.

완료 항목: #2, #10, #11, #68, #70, #71, #72, #92, #93, #94, #95, #96, #69

#97에서 처리한 하위 항목(②④⑧) + WONTFIX 항목(③⑤⑥)도 반영:
- #97 항목 자체는 ①⑦이 미처리로 남으므로 BACKLOG에 유지하되 설명에서 완료 항목 제거

- [ ] **Step 4: docs/Apex_Pipeline.md 로드맵 갱신**

현재 버전 v0.5.8.0 유지 (코드 변경은 있지만 마일스톤 전환은 아님).

- [ ] **Step 5: 커밋**

```bash
git add docs/BACKLOG.md docs/BACKLOG_HISTORY.md docs/Apex_Pipeline.md
git commit -m "docs: backlog sweep 15건 완료 — BACKLOG_HISTORY 이전 + 로드맵 갱신"
```

---

## 빌드 & 검증 요약

| 체크포인트 | 실행 시점 |
|------------|-----------|
| clang-format | Task 11 Step 1 (최종 일괄) |
| 빌드 (debug) | 각 Task 완료 후 또는 최소 Task 1-9 완료 후 1회 |
| 유닛 테스트 71개 | 빌드 시 자동 실행 |
| E2E 11개 | Docker 환경 필요 — CI에서 검증 |
