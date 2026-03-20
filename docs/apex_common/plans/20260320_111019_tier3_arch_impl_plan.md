# Tier 3 아키텍처 정비 — 구현 계획서

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 타입 안전성 강화, 레이어 경계 정리, 프레임워크 캡슐화 확립을 위한 백로그 6건(#91, #89, #3, #66, #56, #90) 일괄 구현

**Architecture:** Phase 1(SessionId 강타입) → Phase 2(core→shared 의존 해소 + FrameType concept) → Phase 3(CoreEngine tracked spawn + io_context 캡슐화) → Phase 4(ErrorCode 서비스 에러 분리) 순서로 진행. 각 Phase는 이전 Phase에 의존.

**Tech Stack:** C++23, Boost.Asio, FlatBuffers, GTest, CMake+Ninja, MSVC(/W4 /WX)

**스펙 문서:** `docs/apex_common/plans/20260320_104709_tier3_arch_design.md`

**사전 필독:**
- `CLAUDE.md` (루트) — 빌드/Git/설계 원칙
- `apex_core/CLAUDE.md` — 빌드 상세, MSVC 주의사항
- `docs/apex_core/apex_core_guide.md` — 프레임워크 API 가이드

**빌드 명령:** `"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug` (반드시 `run_in_background: true`, timeout 설정 금지)

**포맷팅:** 코드 변경 후 빌드 전에 반드시 실행:
```bash
find apex_core apex_shared apex_services \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) ! -name '*_generated.h' | xargs clang-format -i
```

---

## 파일 변경 매핑

### 생성 파일

| 파일 | 용도 |
|------|------|
| `apex_core/include/apex/core/kafka_message_meta.hpp` | core 레이어 Kafka 메타데이터 경량 구조체 |
| `apex_services/gateway/include/apex/gateway/gateway_error.hpp` | Gateway 서비스 전용 에러 enum |
| `apex_services/auth-svc/include/apex/auth_svc/auth_error.hpp` | Auth 서비스 전용 에러 enum |

### 삭제 파일

| 파일 | 이유 |
|------|------|
| `apex_shared/lib/protocols/tcp/include/apex/shared/protocols/tcp/wire_header.hpp` | core로 복원 |
| `apex_shared/lib/protocols/tcp/include/apex/shared/protocols/tcp/frame_codec.hpp` | core로 복원 |

### 수정 파일 (Phase별)

**Phase 1 (#91 SessionId):** ~14개 파일 기계적 치환
**Phase 2 (#89+#3):** ~25개 파일 (헤더 이동 + concept + KafkaMessageMeta + Auth/Chat 핸들러 시그니처 + tcp_binary_protocol include 마이그레이션 9건)
**Phase 3 (#66+#56):** ~4개 파일 (CoreEngine + ServiceBase + kafka_adapter)
**Phase 4 (#90):** ~15개 파일 (ErrorCode + ErrorSender + FBS스키마 + Gateway/Auth)

---

## Phase 1: #91 SessionId 강타입화

### Task 1: SessionId enum class 정의 + 유틸리티

**Files:**
- Modify: `apex_core/include/apex/core/session.hpp:22-23`

- [ ] **Step 1: SessionId를 enum class로 변경**

`session.hpp`에서 기존 `using SessionId = uint64_t;` (라인 23)을 아래로 교체:

```cpp
/// 고유 세션 식별자 (코어별 단조 증가)
enum class SessionId : uint64_t
{
};

/// SessionId 생성 헬퍼
constexpr SessionId make_session_id(uint64_t v) noexcept
{
    return static_cast<SessionId>(v);
}

/// SessionId → uint64_t 변환
constexpr uint64_t to_underlying(SessionId id) noexcept
{
    return static_cast<uint64_t>(id);
}
```

- [ ] **Step 2: std::hash 특수화 추가**

`session.hpp` 파일 끝(`namespace apex::core` 닫는 괄호 바로 뒤)에 추가:

```cpp
template <> struct std::hash<apex::core::SessionId>
{
    std::size_t operator()(apex::core::SessionId id) const noexcept
    {
        return std::hash<uint64_t>{}(apex::core::to_underlying(id));
    }
};
```

- [ ] **Step 3: operator<< 추가 (로깅용)**

`session.hpp`의 `namespace apex::core` 내부, SessionId 정의 바로 아래에 추가:

```cpp
inline std::ostream& operator<<(std::ostream& os, SessionId id)
{
    return os << to_underlying(id);
}
```

**주의:** `<ostream>` include 추가 필요. 기존 includes에 이미 있는지 확인.

- [ ] **Step 4: fmt::formatter 특수화 추가 (spdlog용)**

spdlog는 내부적으로 `fmt::formatter`를 사용. `operator<<`만으로는 `spdlog::info("sid={}", session_id)` 패턴이 컴파일되지 않음. `session.hpp` 파일 끝에 추가:

```cpp
#include <fmt/format.h>

template <> struct fmt::formatter<apex::core::SessionId> : fmt::formatter<uint64_t>
{
    auto format(apex::core::SessionId id, fmt::format_context& ctx) const
    {
        return fmt::formatter<uint64_t>::format(apex::core::to_underlying(id), ctx);
    }
};
```

### Task 2: SessionId 마이그레이션 — apex_core

**Files:**
- Modify: `apex_core/include/apex/core/session_manager.hpp`
- Modify: `apex_core/include/apex/core/connection_handler.hpp`
- Modify: `apex_core/src/session.cpp`
- Modify: `apex_core/src/session_manager.cpp`
- Modify: `apex_core/src/server.cpp`
- Modify: `apex_core/tests/unit/test_session_manager.cpp`
- Modify: `apex_core/tests/unit/test_service_lifecycle.cpp`

- [ ] **Step 1: session_manager.hpp 마이그레이션**

`SessionManager` 내부에서 SessionId를 생성하는 카운터(`next_id_`)가 `uint64_t`일 수 있음. 확인 후:
- `next_id_`가 `uint64_t`이면 유지, 반환 시 `make_session_id(next_id_++)` 사용
- `sessions_` 맵의 키 타입은 이미 `SessionId`이므로 `std::hash` 특수화로 자동 해결

- [ ] **Step 2: connection_handler.hpp 마이그레이션**

`session->id()` 반환 타입이 `SessionId`로 변경됨. 로깅에서 `session->id()`를 직접 출력하면 `operator<<` 자동 적용. `uint64_t`로 산술 연산하는 곳은 `to_underlying()` 경유.

- [ ] **Step 3: session.cpp, session_manager.cpp, server.cpp 마이그레이션**

`SessionId` 생성, 비교, 로깅 부분을 `make_session_id()`, `to_underlying()` 사용으로 변경.

- [ ] **Step 4: 테스트 파일 마이그레이션**

`test_session_manager.cpp`, `test_service_lifecycle.cpp`에서 `SessionId` 리터럴 사용 부분을 `make_session_id(N)` 으로 변경.

### Task 3: SessionId 마이그레이션 — apex_services

**Files:**
- Modify: `apex_services/gateway/include/apex/gateway/channel_session_map.hpp`
- Modify: `apex_services/gateway/include/apex/gateway/gateway_service.hpp`
- Modify: `apex_services/gateway/include/apex/gateway/pending_requests.hpp`
- Modify: `apex_services/gateway/src/gateway_service.cpp`
- Modify: `apex_services/gateway/src/channel_session_map.cpp`
- Modify: `apex_services/gateway/src/pending_requests.cpp`

- [ ] **Step 1: Gateway 헤더 마이그레이션**

`channel_session_map.hpp`: `SessionId`를 맵 키로 사용하는 패턴 — `std::hash` 특수화로 자동 해결, 코드 변경 불필요할 가능성 높음. `SessionId`를 `uint64_t`로 직접 캐스팅하는 곳만 `to_underlying()` 으로 변경.

`pending_requests.hpp`: `SessionId`를 키로 쓰는 맵. 동일 패턴.

`gateway_service.hpp`: `auth_states_` 맵의 키 타입. 동일 패턴.

- [ ] **Step 2: Gateway 소스 마이그레이션**

`gateway_service.cpp`, `channel_session_map.cpp`, `pending_requests.cpp`에서:
- `SessionId` 리터럴 생성 → `make_session_id()`
- `SessionId` → `uint64_t` 변환 (로깅, Kafka 메시지 등) → `to_underlying()`
- 비교 연산 → `enum class` 기본 `==`/`<=>` 사용

**주의:** Kafka envelope의 `session_id` 필드는 wire boundary이므로 `uint64_t` 유지. `to_underlying(session->id())` 패턴으로 변환.

### Task 4: Phase 1 빌드 + 테스트 검증

- [ ] **Step 1: clang-format 실행**

```bash
find apex_core apex_shared apex_services \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) ! -name '*_generated.h' | xargs clang-format -i
```

- [ ] **Step 2: 빌드 실행** (run_in_background: true)

```bash
"<절대경로>/apex_tools/queue-lock.sh" build debug
```

- [ ] **Step 3: 컴파일 에러가 있으면 수정**

`enum class`는 `uint64_t`와 암묵적 변환이 불가하므로 누락된 `make_session_id()`/`to_underlying()` 호출을 추가.

- [ ] **Step 4: 커밋**

```bash
git add -A
git commit -m "feat(core): BACKLOG-91 SessionId 강타입화 — enum class + hash + formatter"
git push
```

---

## Phase 2: #89 core→shared 역방향 의존 해소 + #3 FrameType concept

### Task 5: WireHeader/FrameCodec core 복원

**Files:**
- Modify: `apex_core/include/apex/core/wire_header.hpp` — forwarding → 실제 구현
- Modify: `apex_core/include/apex/core/frame_codec.hpp` — forwarding → 실제 구현
- Modify: `apex_core/include/apex/core/tcp_binary_protocol.hpp` — forwarding 유지, include 경로 갱신
- Delete: `apex_shared/lib/protocols/tcp/include/apex/shared/protocols/tcp/wire_header.hpp`
- Delete: `apex_shared/lib/protocols/tcp/include/apex/shared/protocols/tcp/frame_codec.hpp`
- Modify: `apex_shared/lib/protocols/tcp/include/apex/shared/protocols/tcp/tcp_binary_protocol.hpp` — include 경로 변경
- Modify: `apex_shared/lib/protocols/tcp/CMakeLists.txt`
- Modify: `apex_core/src/wire_header.cpp` — namespace 변경
- Modify: `apex_core/src/frame_codec.cpp` — namespace 변경
- Modify: (9개 파일) `apex_core/examples/{echo_server,multicore_echo_server,chat_server}.cpp`, `apex_core/tests/unit/{test_tcp_binary_protocol,test_server_multicore,test_server_error_paths,test_cross_core_call}.cpp`, `apex_core/tests/integration/{test_server_e2e,test_shutdown_timeout}.cpp` — include 경로를 `<apex/shared/protocols/tcp/tcp_binary_protocol.hpp>`로 변경

- [ ] **Step 1: shared의 WireHeader/FrameCodec 헤더 내용을 core 헤더로 복사**

`apex_shared/lib/protocols/tcp/include/apex/shared/protocols/tcp/wire_header.hpp` 내용을 `apex_core/include/apex/core/wire_header.hpp`로 복사. 네임스페이스를 `apex::shared::protocols::tcp` → `apex::core`로 변경. forwarding header의 using 선언은 제거.

`frame_codec.hpp`도 동일하게 처리.

- [ ] **Step 2: core의 .cpp 파일 namespace 변경**

`apex_core/src/wire_header.cpp`: include를 `<apex/core/wire_header.hpp>`로, namespace를 `apex::core`로 변경.
`apex_core/src/frame_codec.cpp`: 동일.

- [ ] **Step 3: tcp_binary_protocol.hpp forwarding 갱신**

`apex_core/include/apex/core/tcp_binary_protocol.hpp`는 **삭제하지 않고 유지**. TcpBinaryProtocol은 shared의 Protocol 구현이지만, core의 examples(3개)과 tests(6개)가 이 경로를 사용 중:
- `apex_core/examples/echo_server.cpp`, `multicore_echo_server.cpp`, `chat_server.cpp`
- `apex_core/tests/unit/test_tcp_binary_protocol.cpp`, `test_server_multicore.cpp`, `test_server_error_paths.cpp`, `test_cross_core_call.cpp`
- `apex_core/tests/integration/test_server_e2e.cpp`, `test_shutdown_timeout.cpp`

forwarding header 내용은 기존과 동일 (shared로 forward). WireHeader/FrameCodec의 forwarding은 이미 제거되었으므로 이 파일만 남기는 것이 일관적.

**대안:** 9개 파일의 include를 `<apex/shared/protocols/tcp/tcp_binary_protocol.hpp>`로 직접 변경 후 forwarding 삭제도 가능. 구현 시 examples/tests에서 사용하는 namespace 별칭도 함께 정리 필요 여부 확인.

- [ ] **Step 4: shared의 원본 WireHeader/FrameCodec 헤더 삭제**

`apex_shared/lib/protocols/tcp/include/apex/shared/protocols/tcp/wire_header.hpp` 삭제.
`apex_shared/lib/protocols/tcp/include/apex/shared/protocols/tcp/frame_codec.hpp` 삭제.

- [ ] **Step 5: TcpBinaryProtocol include 경로 변경**

`apex_shared/lib/protocols/tcp/include/apex/shared/protocols/tcp/tcp_binary_protocol.hpp`에서:
- `#include <apex/shared/protocols/tcp/wire_header.hpp>` → `#include <apex/core/wire_header.hpp>`
- `#include <apex/shared/protocols/tcp/frame_codec.hpp>` → `#include <apex/core/frame_codec.hpp>`
- 기존 `using` 선언이 있다면 `apex::core::WireHeader`, `apex::core::Frame` 등으로 변경

- [ ] **Step 6: CMakeLists.txt 정리**

`apex_shared/lib/protocols/tcp/CMakeLists.txt`에서 WireHeader/FrameCodec 관련 헤더 참조 제거 (INTERFACE 라이브러리이므로 소스 목록은 원래 없음).

`apex_core/CMakeLists.txt`는 wire_header.cpp, frame_codec.cpp가 이미 소스 목록에 포함되어 있으므로 변경 불필요 — 빌드 시 확인.

- [ ] **Step 7: session.hpp include 확인**

`session.hpp`가 `<apex/core/wire_header.hpp>`과 `<apex/core/frame_codec.hpp>`을 include — 이미 core 경로이므로 변경 불필요. 다만 forwarding에서 실제 구현으로 바뀌었으므로 정상 작동 확인.

### Task 6: KafkaMessageMeta + service_base.hpp kafka 의존 해소

**Files:**
- Create: `apex_core/include/apex/core/kafka_message_meta.hpp`
- Modify: `apex_core/include/apex/core/service_base.hpp` — KafkaHandler 시그니처 변경, kafka_envelope include 제거
- Modify: `apex_shared/lib/protocols/kafka/include/apex/shared/protocols/kafka/kafka_dispatch_bridge.hpp` — Handler typedef 변경
- Modify: `apex_shared/lib/protocols/kafka/src/kafka_dispatch_bridge.cpp` — MetadataPrefix→KafkaMessageMeta 변환
- Modify: `apex_services/auth-svc/include/apex/auth_svc/auth_service.hpp` — 핸들러 파라미터 타입 변경 (6개 메서드: on_login/logout/refresh_token + send_*_error 3개)
- Modify: `apex_services/auth-svc/src/auth_service.cpp` — 핸들러 구현 파라미터 변경 (6개 메서드)
- Modify: `apex_services/chat-svc/include/apex/chat_svc/chat_service.hpp` — 핸들러 파라미터 타입 변경 (~18개 메서드)
- Modify: `apex_services/chat-svc/src/chat_service.cpp` — 핸들러 구현 파라미터 변경 (~18개 메서드)
- Modify: `apex_services/auth-svc/tests/unit/test_auth_handlers.cpp` — 핸들러 직접 호출 시 KafkaMessageMeta 사용
- Modify: `apex_services/chat-svc/tests/unit/test_chat_handlers.cpp` — 핸들러 직접 호출 시 KafkaMessageMeta 사용

- [ ] **Step 1: KafkaMessageMeta 구조체 생성**

```cpp
// apex_core/include/apex/core/kafka_message_meta.hpp
#pragma once

#include <cstdint>

namespace apex::core
{

/// Kafka 메시지 메타데이터 — core 레이어용 경량 구조체.
/// shared의 MetadataPrefix와 1:1 대응. core가 kafka_envelope.hpp에 의존하지 않도록 분리.
struct KafkaMessageMeta
{
    uint32_t meta_version{0};
    uint16_t core_id{0};
    uint64_t corr_id{0};
    uint16_t source_id{0};
    uint64_t session_id{0};
    uint64_t user_id{0};
    uint64_t timestamp{0};
};

} // namespace apex::core
```

- [ ] **Step 2: service_base.hpp KafkaHandler 시그니처 변경**

`service_base.hpp`에서:
1. `#include <apex/shared/protocols/kafka/kafka_envelope.hpp>` 삭제
2. `#include <apex/core/kafka_message_meta.hpp>` 추가
3. `ServiceBaseInterface`의 `KafkaHandler` 타입 변경:

```cpp
// 변경 전
using KafkaHandler = std::function<boost::asio::awaitable<Result<void>>(
    shared::protocols::kafka::MetadataPrefix, uint32_t, std::span<const uint8_t>)>;

// 변경 후
using KafkaHandler = std::function<boost::asio::awaitable<Result<void>>(
    KafkaMessageMeta, uint32_t, std::span<const uint8_t>)>;
```

4. `kafka_route<T>()`의 핸들러 시그니처도 동일하게 변경:

```cpp
// 변경 전
void kafka_route(uint32_t msg_id, awaitable<Result<void>> (Derived::*method)(
    const shared::protocols::kafka::MetadataPrefix&, uint32_t, const FbsType*));

// 변경 후
void kafka_route(uint32_t msg_id, awaitable<Result<void>> (Derived::*method)(
    const KafkaMessageMeta&, uint32_t, const FbsType*));
```

- [ ] **Step 3: kafka_dispatch_bridge.hpp Handler typedef 변경**

`kafka_dispatch_bridge.hpp`에서:
1. `#include <apex/core/kafka_message_meta.hpp>` 추가
2. Handler typedef를 `MetadataPrefix` → `KafkaMessageMeta`로 변경:

```cpp
// 변경 전
using Handler = std::function<boost::asio::awaitable<apex::core::Result<void>>(MetadataPrefix, uint32_t,
                                                                               std::span<const uint8_t>)>;
// 변경 후
using Handler = std::function<boost::asio::awaitable<apex::core::Result<void>>(apex::core::KafkaMessageMeta, uint32_t,
                                                                               std::span<const uint8_t>)>;
```

3. `kafka_envelope.hpp` include는 유지 — dispatch()에서 MetadataPrefix 파싱에 여전히 사용.

- [ ] **Step 4: kafka_dispatch_bridge.cpp 변환 로직 추가**

`kafka_dispatch_bridge.cpp`에서 핸들러 호출 직전에 `MetadataPrefix` → `KafkaMessageMeta` 변환:

```cpp
#include <apex/core/kafka_message_meta.hpp>

// dispatch() 내부, 핸들러 호출 직전
apex::core::KafkaMessageMeta meta{
    .meta_version = metadata.meta_version,
    .core_id = metadata.core_id,
    .corr_id = metadata.corr_id,
    .source_id = metadata.source_id,
    .session_id = metadata.session_id,
    .user_id = metadata.user_id,
    .timestamp = metadata.timestamp,
};
co_return co_await it->second(meta, routing.msg_id, payload);
```

- [ ] **Step 5: Auth/Chat 서비스 핸들러 시그니처 업데이트**

영향 범위가 큼. 정확한 파일별 변경:

**Auth 서비스:**
- `auth_service.hpp`: 8개 메서드 선언의 `const envelope::MetadataPrefix&` → `const apex::core::KafkaMessageMeta&` 파라미터 변경. `using envelope = apex::shared::protocols::kafka;` 별칭이 있다면 `#include <apex/core/kafka_message_meta.hpp>` 추가.
- `auth_service.cpp`: 8개 메서드 정의의 동일 파라미터 변경. `meta.session_id` 등 필드 접근은 KafkaMessageMeta도 동일 필드명이므로 변경 불필요.

**Chat 서비스:**
- `chat_service.hpp`: ~18개 메서드 선언의 파라미터 변경 (on_create_room, on_join_room, on_leave_room, on_list_rooms, on_send_message, on_whisper, on_chat_history, on_global_broadcast + send_*_error 헬퍼 8개).
- `chat_service.cpp`: ~18개 메서드 정의의 동일 파라미터 변경.

**`kafka_envelope.hpp` include 제거 여부:** Auth/Chat이 MetadataPrefix를 직접 생성/파싱하는 곳이 있으면 유지. 핸들러 파라미터로만 받는다면 제거 가능. 구현 시 확인.

- [ ] **Step 6: Auth/Chat 테스트 파일 업데이트**

테스트에서 핸들러를 직접 호출하거나 MetadataPrefix로 KafkaMessageMeta를 대체해야 하는 곳:
- `test_auth_handlers.cpp`: MetadataPrefix를 구성하여 envelope 빌드하는 코드는 **변경 불필요** (envelope 파싱은 bridge가 담당). 단, 핸들러를 직접 호출하는 테스트가 있으면 KafkaMessageMeta로 변경.
- `test_chat_handlers.cpp`: 동일 패턴.
- `test_envelope_codec.cpp`: MetadataPrefix 직접 테스트이므로 **변경 불필요** (이건 shared 레벨 테스트).

### Task 7: FrameType concept + payload() accessor

**Files:**
- Modify: `apex_core/include/apex/core/protocol.hpp` — FrameType concept 추가
- Modify: `apex_core/include/apex/core/frame_codec.hpp` — Frame에 payload() accessor 추가
- Modify: `apex_shared/lib/protocols/websocket/include/apex/shared/protocols/websocket/websocket_protocol.hpp` — payload() accessor 추가
- Modify: `apex_shared/lib/protocols/websocket/src/websocket_protocol.cpp` — `frame.payload` → `frame.payload_data` 변경 (2곳)
- Modify: `apex_core/include/apex/core/connection_handler.hpp` — payload() accessor 사용 전환

- [ ] **Step 1: FrameType concept 추가**

`protocol.hpp`에서 기존 Protocol concept 위에 FrameType concept 추가:

```cpp
/// Frame 타입 제약 — payload() accessor를 요구.
/// msg_id 추출 방식은 프로토콜마다 다르므로 concept에 포함하지 않음.
template <typename F>
concept FrameType = requires(const F& f) {
    { f.payload() } -> std::convertible_to<std::span<const uint8_t>>;
};

template <typename P>
concept Protocol = requires {
    typename P::Config;
    typename P::Frame;
} && FrameType<typename P::Frame>
  && requires(RingBuffer& buf, const typename P::Frame& frame) {
    { P::try_decode(buf) } -> std::same_as<Result<typename P::Frame>>;
    { P::consume_frame(buf, frame) } -> std::same_as<void>;
};
```

`<span>` include 추가 필요.

- [ ] **Step 2: Frame에 payload() accessor 추가**

`frame_codec.hpp` (core로 복원된 버전)의 `Frame` 구조체에:

```cpp
struct Frame
{
    WireHeader header;
    std::span<const uint8_t> payload; // 기존 public 멤버 유지

    /// FrameType concept 만족용 accessor
    [[nodiscard]] std::span<const uint8_t> payload() const noexcept
    {
        return payload;
    }
};
```

**주의:** 멤버와 메서드 이름이 같으면 충돌. 멤버를 `payload_data`로 리네이밍하거나, accessor를 다른 이름으로 하는 방법도 있지만 — **기존 코드 호환성을 위해 멤버를 `payload_data`로 리네이밍**하고 `payload()` accessor가 `payload_data`를 반환하도록:

```cpp
struct Frame
{
    WireHeader header;
    std::span<const uint8_t> payload_data;

    [[nodiscard]] std::span<const uint8_t> payload() const noexcept
    {
        return payload_data;
    }
};
```

기존 `frame.payload` 사용하는 곳을 `frame.payload()` 또는 `frame.payload_data`로 변경.

- [ ] **Step 3: WebSocketProtocol::Frame에 payload() accessor 추가**

`websocket_protocol.hpp`의 Frame 구조체에:

```cpp
struct Frame
{
    std::vector<uint8_t> payload_data; // 리네이밍 (기존: payload)
    bool is_text = false;
    bool is_binary = true;

    [[nodiscard]] std::span<const uint8_t> payload() const noexcept
    {
        return {payload_data.data(), payload_data.size()};
    }
};
```

- [ ] **Step 4: MockProtocol::Frame 갱신**

`protocol.hpp`의 `detail::MockProtocol::Frame`에 payload() accessor 추가:

```cpp
struct Frame
{
    [[nodiscard]] std::span<const uint8_t> payload() const noexcept
    {
        return {};
    }
};
```

- [ ] **Step 5: connection_handler.hpp에서 payload() accessor 사용 전환**

`frame.payload` → `frame.payload()` 로 변경. 해당 파일 전체에서 검색 후 치환. `frame.header.msg_id` 패턴은 유지 (TCP 전용, `if constexpr` 분기 내부).

- [ ] **Step 6: websocket_protocol.cpp payload 멤버명 변경**

`apex_shared/lib/protocols/websocket/src/websocket_protocol.cpp`에서:
- 라인 41: `frame.payload.assign(...)` → `frame.payload_data.assign(...)`
- 라인 51: `frame.payload.size()` → `frame.payload_data.size()`

WebSocket의 try_decode()에서 Frame을 직접 구성하므로 멤버명 변경 필수.

- [ ] **Step 7: 기타 frame.payload 사용처 치환**

`grep -rn "frame\.payload[^_(]" apex_core/ apex_shared/ apex_services/`로 찾아서 모두 `frame.payload()` 또는 `frame.payload_data`로 변경. **examples/ 디렉토리도 포함.**

### Task 8: Phase 2 빌드 + 테스트 + 커밋

- [ ] **Step 1: clang-format 실행**
- [ ] **Step 2: 빌드 실행** (run_in_background: true)
- [ ] **Step 3: 컴파일 에러 수정** — 특히 namespace 변경, include 경로, 멤버 리네이밍으로 인한 에러
- [ ] **Step 4: 의존성 확인**

core→shared 물리적 의존이 0건인지 확인 (라이브러리 코드만 — examples/tests 제외):
```bash
grep -rn "apex/shared" apex_core/include/ apex_core/src/ | grep -v "test" | grep -v "_generated"
```
결과가 0건이어야 함.

**참고:** `apex_core/examples/`와 `apex_core/tests/`는 라이브러리 소비자이므로 shared 의존 허용. `tcp_binary_protocol.hpp` forwarding이 남아있어 이 경로를 사용.

- [ ] **Step 5: 커밋**

```bash
git add -A
git commit -m "refactor(core,shared): BACKLOG-89,3 core→shared 역방향 의존 해소 + FrameType concept"
git push
```

---

## Phase 3: #66 CoreEngine tracked spawn + #56 io_context 캡슐화

### Task 9: CoreEngine::spawn_tracked() 추가

**Files:**
- Modify: `apex_core/include/apex/core/core_engine.hpp`
- Modify: `apex_core/src/core_engine.cpp`

- [ ] **Step 1: outstanding 카운터 추가**

`core_engine.hpp`의 CoreEngine private 멤버에:

```cpp
std::atomic<uint32_t> outstanding_infra_coros_{0};
```

- [ ] **Step 2: spawn_tracked 템플릿 메서드 추가**

`core_engine.hpp`의 CoreEngine public 영역에:

```cpp
/// [D7] Tracked 인프라 코루틴 스폰. 서비스 코루틴과 별도 추적.
/// shutdown 시 완료 대기 (서비스 코루틴 이후).
template <typename F>
void spawn_tracked(uint32_t core_id, F&& coro_factory)
{
    assert(core_id < core_count() && "Invalid core_id for spawn_tracked");
    outstanding_infra_coros_.fetch_add(1, std::memory_order_acq_rel);
    boost::asio::co_spawn(
        io_context(core_id),
        [this, f = std::forward<F>(coro_factory)]() -> boost::asio::awaitable<void> {
            try
            {
                co_await f();
            }
            catch (const std::exception& e)
            {
                spdlog::error("[CoreEngine] spawn_tracked coroutine exception: {}", e.what());
            }
            outstanding_infra_coros_.fetch_sub(1, std::memory_order_acq_rel);
        },
        boost::asio::detached);
}

/// Outstanding 인프라 코루틴 수.
[[nodiscard]] uint32_t outstanding_infra_coroutines() const noexcept
{
    return outstanding_infra_coros_.load(std::memory_order_acquire);
}
```

`spdlog/spdlog.h` include 필요 — `spawn_tracked`의 catch 블록에서 `spdlog::error()` 사용. 기존 includes에 이미 있는지 확인 후 없으면 추가.

- [ ] **Step 3: (선택) shutdown 시 인프라 코루틴 대기 로직**

기존 `stop()`/`join()` 로직에 outstanding_infra_coros_ 대기를 넣을지 결정. 현재 설계에서는 "서비스 코루틴 → 인프라 코루틴" 순서 대기인데, 이는 Server::run() 레벨에서 오케스트레이션하는 것이 자연스러움. CoreEngine 자체에는 카운터 노출만 하고, 대기 로직은 Server가 담당.

### Task 10: kafka_adapter.cpp co_spawn(detached) → spawn_tracked

**Files:**
- Modify: `apex_shared/lib/adapters/kafka/src/kafka_adapter.cpp:183-193`

- [ ] **Step 1: co_spawn(detached) → engine.spawn_tracked(0, ...) 변경**

```cpp
// 변경 전 (라인 184-193)
boost::asio::co_spawn(
    engine.io_context(0),
    [bridge, buf = std::move(pooled_buf)]() -> boost::asio::awaitable<void> {
        auto result = co_await bridge->dispatch(buf->span());
        if (!result.has_value())
            spdlog::warn("[KafkaAdapter] auto-wired dispatch failed: {}", static_cast<int>(result.error()));
    },
    boost::asio::detached);

// 변경 후
engine.spawn_tracked(0, [bridge, buf = std::move(pooled_buf)]() -> boost::asio::awaitable<void> {
    auto result = co_await bridge->dispatch(buf->span());
    if (!result.has_value())
        spdlog::warn("[KafkaAdapter] auto-wired dispatch failed: {}", static_cast<int>(result.error()));
});
```

`#include <apex/core/core_engine.hpp>` 가 이미 있는지 확인. wire_services()가 `CoreEngine&`을 받으므로 있을 것.

### Task 11: ServiceBase io_ctx_ private + post()/get_executor()

**Files:**
- Modify: `apex_core/include/apex/core/service_base.hpp`

- [ ] **Step 1: post() 템플릿 메서드 추가**

**참고:** `io_ctx_`는 이미 private (라인 354). 별도 이동 불필요.

`service_base.hpp`의 protected 영역 (`spawn()` 바로 아래)에:

```cpp
/// io_context에 작업 게시. io_context 직접 접근 대신 사용.
template <typename F>
void post(F&& fn)
{
    assert(io_ctx_ && "post() called before internal_configure");
    boost::asio::post(*io_ctx_, std::forward<F>(fn));
}

/// io_context의 executor 반환. timer 등에 필요.
[[nodiscard]] boost::asio::any_io_executor get_executor() noexcept
{
    assert(io_ctx_ && "get_executor() called before internal_configure");
    return io_ctx_->get_executor();
}
```

`<boost/asio/post.hpp>`, `<boost/asio/any_io_executor.hpp>` include 추가 (기존 includes에 이미 있을 수 있음).

### Task 12: Phase 3 빌드 + 테스트 + 커밋

- [ ] **Step 1: clang-format 실행**
- [ ] **Step 2: 빌드 실행** (run_in_background: true)
- [ ] **Step 3: 컴파일 에러 수정**
- [ ] **Step 4: 커밋**

```bash
git add -A
git commit -m "feat(core): BACKLOG-66,56 CoreEngine spawn_tracked + ServiceBase io_context 캡슐화"
git push
```

---

## Phase 4: #90 ErrorCode Gateway 에러 분리

### Task 13: ErrorCode ServiceError sentinel + GatewayError/AuthError enum

**Files:**
- Modify: `apex_core/include/apex/core/error_code.hpp`
- Create: `apex_services/gateway/include/apex/gateway/gateway_error.hpp`
- Create: `apex_services/auth-svc/include/apex/auth_svc/auth_error.hpp`

- [ ] **Step 1: ErrorCode에서 Gateway 에러 제거 + ServiceError 추가**

`error_code.hpp`에서:

```cpp
enum class ErrorCode : uint16_t
{
    Ok = 0,

    // Framework errors (1-98)
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
    HandlerException = 11,
    SendFailed = 12,
    InsufficientData = 13,
    AcceptFailed = 14,
    HandshakeFailed = 15,
    ParseFailed = 16,

    // 서비스별 에러 sentinel
    ServiceError = 99,

    // Application errors (1000-1999)
    AppError = 1000,

    // Adapter errors (2000+)
    AdapterError = 2000,
    PoolExhausted = 2001,
    CircuitOpen = 2002,
};
```

`error_code_name()` 함수에서:
- Gateway 에러 case 11개 삭제
- `ServiceError` case 추가: `return "ServiceError";`

- [ ] **Step 2: GatewayError enum 생성**

```cpp
// apex_services/gateway/include/apex/gateway/gateway_error.hpp
#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>

namespace apex::gateway
{

enum class GatewayError : uint16_t
{
    ConfigParseFailed = 1,
    JwtVerifyFailed = 2,
    JwtExpired = 3,
    JwtBlacklisted = 4,
    RouteNotFound = 5,
    ServiceTimeout = 6,
    PendingMapFull = 7,
    RateLimitedIp = 8,
    RateLimitedUser = 9,
    RateLimitedEndpoint = 10,
    SubscriptionLimitExceeded = 11,
};

constexpr std::string_view gateway_error_name(GatewayError code) noexcept
{
    switch (code)
    {
        case GatewayError::ConfigParseFailed:
            return "ConfigParseFailed";
        case GatewayError::JwtVerifyFailed:
            return "JwtVerifyFailed";
        case GatewayError::JwtExpired:
            return "JwtExpired";
        case GatewayError::JwtBlacklisted:
            return "JwtBlacklisted";
        case GatewayError::RouteNotFound:
            return "RouteNotFound";
        case GatewayError::ServiceTimeout:
            return "ServiceTimeout";
        case GatewayError::PendingMapFull:
            return "PendingMapFull";
        case GatewayError::RateLimitedIp:
            return "RateLimitedIp";
        case GatewayError::RateLimitedUser:
            return "RateLimitedUser";
        case GatewayError::RateLimitedEndpoint:
            return "RateLimitedEndpoint";
        case GatewayError::SubscriptionLimitExceeded:
            return "SubscriptionLimitExceeded";
        default:
            return "Unknown";
    }
}

inline std::ostream& operator<<(std::ostream& os, GatewayError ec)
{
    return os << gateway_error_name(ec);
}

} // namespace apex::gateway
```

- [ ] **Step 3: AuthError enum 생성**

```cpp
// apex_services/auth-svc/include/apex/auth_svc/auth_error.hpp
#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>

namespace apex::auth_svc
{

enum class AuthError : uint16_t
{
    JwtVerifyFailed = 1,
};

constexpr std::string_view auth_error_name(AuthError code) noexcept
{
    switch (code)
    {
        case AuthError::JwtVerifyFailed:
            return "JwtVerifyFailed";
        default:
            return "Unknown";
    }
}

inline std::ostream& operator<<(std::ostream& os, AuthError ec)
{
    return os << auth_error_name(ec);
}

} // namespace apex::auth_svc
```

### Task 14: ErrorSender + ErrorResponse 스키마 확장

**Files:**
- Modify: `apex_core/schemas/error_response.fbs` — TCP wire 에러 스키마
- Modify: `apex_core/include/apex/core/error_sender.hpp`
- Modify: `apex_core/src/error_sender.cpp`

**중요:** ErrorSender는 `error_response.fbs` (apex_core, TCP wire용)를 사용 — `error_envelope.fbs` (apex_shared, Kafka용)가 아님. `error_sender.cpp`가 `#include <generated/error_response_generated.h>`를 사용하고 `apex::messages::CreateErrorResponse`를 호출하는 것으로 확인됨.

- [ ] **Step 1: ErrorResponse FlatBuffers 스키마에 service_error_code 추가**

`apex_core/schemas/error_response.fbs`:
```
namespace apex.messages;

file_identifier "AERR";

table ErrorResponse {
    code:uint16;
    message:string;
    service_error_code:uint16;  // 새 필드 — 테이블 끝에 추가 (하위 호환)
}

root_type ErrorResponse;
```

**FlatBuffers 재생성:** CMake가 빌드 시 자동 처리 (schemas 디렉토리의 .fbs → _generated.h).

- [ ] **Step 2: ErrorSender 시그니처 확장**

`error_sender.hpp`:

```cpp
[[nodiscard]] static std::vector<uint8_t> build_error_frame(
    uint32_t original_msg_id,
    ErrorCode code,
    std::string_view message = "",
    uint16_t service_error_code = 0  // 새 파라미터
);
```

`error_sender.cpp` 구현 변경:

```cpp
auto resp = apex::messages::CreateErrorResponse(
    builder,
    static_cast<uint16_t>(code),
    msg_offset,
    service_error_code  // 새 필드
);
```

- `ErrorCode::ServiceError`일 때 `service_error_code` 값이 의미 있음
- 그 외에는 0으로 전달 (하위 호환, FlatBuffers 기본값 0)

### Task 15: Gateway 서비스 에러 코드 마이그레이션

**Files:**
- Modify: `apex_services/gateway/src/channel_session_map.cpp`
- Modify: `apex_services/gateway/src/gateway_config_parser.cpp`
- Modify: `apex_services/gateway/src/gateway_pipeline.cpp`
- Modify: `apex_services/gateway/src/gateway_service.cpp`
- Modify: `apex_services/gateway/src/jwt_verifier.cpp`
- Modify: `apex_services/gateway/src/message_router.cpp`
- Modify: `apex_services/gateway/src/pending_requests.cpp`
- Modify: `apex_services/gateway/src/route_table.cpp`
- Modify: `apex_services/gateway/tests/test_channel_session_map.cpp` — `ErrorCode::SubscriptionLimitExceeded` → `GatewayError` 변경
- Modify: `apex_services/gateway/tests/test_config_reloader.cpp` — `ErrorCode::ConfigParseFailed` → `GatewayError` 변경

- [ ] **Step 1: 각 파일에 gateway_error.hpp include 추가**

모든 Gateway 소스 파일에:
```cpp
#include <apex/gateway/gateway_error.hpp>
```

- [ ] **Step 2: ErrorCode::XXX → GatewayError::XXX 전환**

패턴별 변환:
- `ErrorCode::ConfigParseFailed` → `GatewayError::ConfigParseFailed`
- `ErrorCode::JwtVerifyFailed` → `GatewayError::JwtVerifyFailed`
- ... (11개 모두)

에러 반환 패턴 변경 — **서비스 에러는 직접 전송 + ok() 반환 패턴**:

`Result<void>`는 `std::expected<void, ErrorCode>` — ErrorCode만 담을 수 있고 service_error_code를 전달할 수 없음. 따라서:

1. **서비스 에러**: 핸들러가 `ErrorSender::build_error_frame(msg_id, ErrorCode::ServiceError, message, static_cast<uint16_t>(GatewayError::XXX))` 으로 에러 프레임을 직접 구성 → 세션에 전송 → `return ok();`
2. **프레임워크 에러**: 기존대로 `return error(ErrorCode::HandlerNotFound)` 등 — connection_handler가 처리

이 패턴은 Auth/Chat 서비스가 이미 사용 중 (`send_*_error()` 헬퍼로 에러 전송 후 `ok()` 반환). Gateway도 동일 패턴으로 정비.

**주의:** Gateway는 TCP 직접 연결이므로 session에 `async_write`로 에러 프레임을 전송. Auth/Chat은 Kafka로 응답.

### Task 16: Auth 서비스 에러 마이그레이션

**Files:**
- Modify: `apex_services/auth-svc/src/jwt_manager.cpp`

- [ ] **Step 1: jwt_manager.cpp의 JwtVerifyFailed 변경**

Auth 서비스는 이미 `send_*_error()` 패턴을 사용 중. `ErrorCode::JwtVerifyFailed`가 Result에 직접 반환되는 곳을 확인:
- 에러 프레임 전송 후 `ok()` 반환 경로면 → 이미 올바른 패턴. `ErrorCode::JwtVerifyFailed` 참조만 `AuthError::JwtVerifyFailed`로 변경.
- `return error(ErrorCode::JwtVerifyFailed)`로 직접 반환하는 곳이면 → `send_*_error()` 호출 후 `return ok()` 패턴으로 변경. 또는 `return error(ErrorCode::ServiceError)` (프레임워크에 "서비스 에러 발생"을 알리되 detail은 이미 전송됨).

**실제 구현 시 패턴 확인:** `jwt_manager.cpp`가 직접 에러 프레임을 전송하는지, Result만 반환하는지에 따라 분기.

### Task 17: Phase 4 빌드 + 테스트 + E2E 검증

- [ ] **Step 1: clang-format 실행**
- [ ] **Step 2: 빌드 실행** (run_in_background: true)
- [ ] **Step 3: 컴파일 에러 수정** — 특히 ErrorCode 변경으로 인한 switch/case 미처리 경고
- [ ] **Step 4: E2E 테스트에서 에러 코드 검증 확인**

E2E 테스트가 특정 ErrorCode 값을 하드코딩하고 있다면 `ServiceError(99)` + `service_error_code` 조합으로 변경.

- [ ] **Step 5: 커밋**

```bash
git add -A
git commit -m "refactor(core,gateway,auth): BACKLOG-90 ErrorCode 서비스 에러 분리 — ServiceError sentinel + GatewayError/AuthError"
git push
```

---

## Phase 5: 문서 갱신 + 최종 검증

### Task 18: 문서 갱신

**Files:**
- Modify: `docs/apex_core/apex_core_guide.md` — ErrorCode 체계, FrameType concept, spawn_tracked, post()/get_executor()
- Modify: `docs/Apex_Pipeline.md` — 로드맵 버전 기록
- Modify: `docs/BACKLOG.md` — #91, #89, #3, #66, #56, #90 완료 처리 → BACKLOG_HISTORY.md로 이전
- Modify: `CLAUDE.md` — 로드맵 현재 버전 갱신
- Modify: `README.md` — 필요 시 갱신

- [ ] **Step 1: apex_core_guide.md 갱신**

다음 섹션 갱신:
- ErrorCode 체계: `ServiceError` sentinel 패턴 설명
- Protocol concept: `FrameType` concept 추가, `payload()` accessor 설명
- ServiceBase API: `spawn()`, `post()`, `get_executor()` 설명
- CoreEngine API: `spawn_tracked()` 설명

- [ ] **Step 2: Apex_Pipeline.md 로드맵 기록**

활성 로드맵 섹션에 버전 추가 (예: v0.5.9.0 Tier 3 아키텍처 정비).

- [ ] **Step 3: BACKLOG.md 완료 처리**

#91, #89, #3, #66, #56, #90 — 6건을 BACKLOG.md에서 삭제, BACKLOG_HISTORY.md에 추가.

- [ ] **Step 4: CLAUDE.md 로드맵 갱신**
- [ ] **Step 5: 커밋**

```bash
git add -A
git commit -m "docs: BACKLOG-91,89,3,66,56,90 Tier 3 아키텍처 정비 문서 갱신"
git push
```

### Task 19: 최종 검증 체크리스트

- [ ] MSVC (`/W4 /WX`) 로컬 빌드 성공
- [ ] CI 파이프라인 전체 통과 (PR 생성 후 확인)
- [ ] clang-format 통과
- [ ] 71+ 유닛 테스트 전체 통과
- [ ] 11 E2E 테스트 전체 통과
- [ ] core→shared 물리적 의존 0건: `grep -rn "apex/shared" apex_core/include/ apex_core/src/ | grep -v test | grep -v _generated` → 0건
- [ ] SessionId 암묵적 변환 불가 확인 (빌드 성공 = 확인 완료)
- [ ] 서비스 코드에서 io_ctx_ 직접 접근 불가 확인 (빌드 성공 = 확인 완료)
