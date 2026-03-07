# Phase 4: 프로토콜 + 세션 구현 계획

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**목표:** FlatBuffers 타입 디스패치, 코루틴 기반 세션 관리(하트비트 타임아웃), 자동 에러 응답 전파 구현

**아키텍처:** Phase 1-3 기반 위에 3개 병렬 컴포넌트 구축. (A) FlatBuffers 코드젠 파이프라인 + 타입 안전 `route<T>()` 디스패치. (B) TCP 소켓 + RingBuffer를 감싸는 Session 클래스, 코어별 SessionManager와 TimingWheel 하트비트 연동. (C) `apex::Result<T>`를 통한 에러 전파 + ErrorResponse 프레임 자동 생성.

**기술 스택:** C++23, Boost.Asio (awaitable, co_spawn), FlatBuffers (flatc 코드젠), 기존 Phase 1-3 컴포넌트 (CoreEngine, ServiceBase, MPSC, SlabPool, RingBuffer, TimingWheel, WireHeader, FrameCodec)

**선행 조건:** Phase 3 완료 (커밋 a4d5132)

---

## 사전 읽기: 핵심 기존 파일

작업 시작 전 반드시 읽어야 할 파일:

| 파일 | 이유 |
|------|------|
| `core/include/apex/core/service_base.hpp` | CRTP 패턴, handle() 등록, 디스패처 |
| `core/include/apex/core/core_engine.hpp` | CoreContext, CoreEngine, MPSC 메시지 라우팅 |
| `core/include/apex/core/wire_header.hpp` | 10바이트 헤더 형식, wire_flags |
| `core/include/apex/core/frame_codec.hpp` | RingBuffer 기반 프레임 디코드/인코드 |
| `core/include/apex/core/timing_wheel.hpp` | EntryId, schedule/cancel/tick API |
| `core/include/apex/core/slab_pool.hpp` | TypedSlabPool<T> 세션 할당용 |
| `core/include/apex/core/ring_buffer.hpp` | writable/commit_write/linearize API |
| `core/examples/echo_server.cpp` | 현재 세션 패턴 (EchoSession) |
| `core/tests/unit/CMakeLists.txt` | apex_add_unit_test() 함수 패턴 |
| `design-decisions.md` | 전체 아키텍처 결정 사항 |

---

## Task 1: 헤더 인터페이스 + CMake 스텁 (순차)

**목적:** Phase 4 공개 API를 먼저 전부 정의. 빈 소스 파일과 주석 처리된 테스트를 생성해서 병렬 에이전트 간 파일 충돌 방지.

**파일:**
- 생성: `core/include/apex/core/result.hpp`
- 생성: `core/include/apex/core/error_code.hpp`
- 생성: `core/include/apex/core/session.hpp`
- 생성: `core/include/apex/core/session_manager.hpp`
- 생성: `core/schemas/echo.fbs`
- 생성: `core/schemas/error_response.fbs`
- 생성: `core/schemas/heartbeat.fbs`
- 생성: `core/src/session.cpp` (빈 스텁)
- 생성: `core/src/session_manager.cpp` (빈 스텁)
- 수정: `core/CMakeLists.txt` (새 소스 추가, FlatBuffers 코드젠)
- 수정: `core/include/apex/core/service_base.hpp` (route<T> 선언 추가)
- 생성: `core/tests/unit/test_session.cpp` (주석 처리)
- 생성: `core/tests/unit/test_session_manager.cpp` (주석 처리)
- 생성: `core/tests/unit/test_error_propagation.cpp` (주석 처리)
- 생성: `core/tests/unit/test_flatbuffers_dispatch.cpp` (주석 처리)

### Step 1: error_code.hpp 생성

```cpp
// core/include/apex/core/error_code.hpp
#pragma once

#include <cstdint>
#include <string_view>

namespace apex::core {

enum class ErrorCode : uint16_t {
    Ok = 0,

    // 프레임워크 에러 (1-999)
    Unknown = 1,
    InvalidMessage = 2,
    HandlerNotFound = 3,
    SessionClosed = 4,
    BufferFull = 5,
    Timeout = 6,
    FlatBuffersVerifyFailed = 7,

    // 어플리케이션 에러 (1000+)
    AppError = 1000,
};

constexpr std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::Unknown: return "Unknown";
        case ErrorCode::InvalidMessage: return "InvalidMessage";
        case ErrorCode::HandlerNotFound: return "HandlerNotFound";
        case ErrorCode::SessionClosed: return "SessionClosed";
        case ErrorCode::BufferFull: return "BufferFull";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::FlatBuffersVerifyFailed: return "FlatBuffersVerifyFailed";
        case ErrorCode::AppError: return "AppError";
        default: return "Unknown";
    }
}

} // namespace apex::core
```

### Step 2: result.hpp 생성

```cpp
// core/include/apex/core/result.hpp
#pragma once

#include <apex/core/error_code.hpp>
#include <expected>

namespace apex::core {

template <typename T = void>
using Result = std::expected<T, ErrorCode>;

/// 핸들러에서 성공 반환용 헬퍼
inline Result<void> ok() { return {}; }

/// 핸들러에서 에러 반환용 헬퍼
inline std::unexpected<ErrorCode> error(ErrorCode code) {
    return std::unexpected(code);
}

} // namespace apex::core
```

### Step 3: session.hpp 생성

```cpp
// core/include/apex/core/session.hpp
#pragma once

#include <apex/core/frame_codec.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/wire_header.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <memory>

namespace apex::core {

/// 고유 세션 식별자 (코어별 단조 증가)
using SessionId = uint64_t;

/// 단일 클라이언트 연결을 나타내는 클래스.
/// SessionManager가 shared_ptr로 소유 (코루틴 안전성 보장).
///
/// 생명주기: Connected -> Active -> Closing -> Closed
/// - Connected: TCP 연결 수립, 아직 인증되지 않음
/// - Active: 정상 운영 상태
/// - Closing: 그레이스풀 셧다운 진행 중 (전송 버퍼 드레인)
/// - Closed: 소켓 종료, 정리 대기
class Session : public std::enable_shared_from_this<Session> {
public:
    enum class State : uint8_t {
        Connected,
        Active,
        Closing,
        Closed,
    };

    Session(SessionId id, boost::asio::ip::tcp::socket socket,
            uint32_t core_id, size_t recv_buf_capacity = 8192);
    ~Session();

    // 복사/이동 불가
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// 프레임 응답을 이 세션에 전송.
    /// 헤더 + 페이로드를 직렬화하여 소켓에 쓴다.
    /// 세션 소유 코어에서만 호출할 것.
    /// 세션이 closed/closing이면 false 반환.
    [[nodiscard]] bool send(const WireHeader& header,
                            std::span<const uint8_t> payload);

    /// 미리 빌드된 로우 프레임 전송 (헤더 이미 직렬화됨).
    [[nodiscard]] bool send_raw(std::span<const uint8_t> data);

    /// 세션 그레이스풀 종료.
    void close();

    // --- 접근자 ---
    [[nodiscard]] SessionId id() const noexcept { return id_; }
    [[nodiscard]] uint32_t core_id() const noexcept { return core_id_; }
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool is_open() const noexcept {
        return state_ == State::Connected || state_ == State::Active;
    }
    [[nodiscard]] boost::asio::ip::tcp::socket& socket() noexcept {
        return socket_;
    }
    [[nodiscard]] RingBuffer& recv_buffer() noexcept { return recv_buf_; }

    /// 상태 변경 (SessionManager가 호출)
    void set_state(State s) noexcept { state_ = s; }

private:
    SessionId id_;
    uint32_t core_id_;
    State state_{State::Connected};
    boost::asio::ip::tcp::socket socket_;
    RingBuffer recv_buf_;
};

using SessionPtr = std::shared_ptr<Session>;

} // namespace apex::core
```

### Step 4: session_manager.hpp 생성

```cpp
// core/include/apex/core/session_manager.hpp
#pragma once

#include <apex/core/session.hpp>
#include <apex/core/timing_wheel.hpp>

#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace apex::core {

/// 코어별 세션 매니저.
/// NOT thread-safe — 단일 코어(io_context-per-core) 전용.
///
/// 역할:
/// - 세션 생성/삭제
/// - SessionId로 세션 추적
/// - TimingWheel 연동 하트비트 타임아웃
/// - 메시지 디스패치를 위한 세션 검색
class SessionManager {
public:
    /// @param core_id 이 매니저가 속한 코어
    /// @param heartbeat_timeout_ticks 타임아웃까지의 틱 수 (0 = 비활성화)
    /// @param timer_wheel_slots 타이밍 휠 슬롯 수
    explicit SessionManager(uint32_t core_id,
                            uint32_t heartbeat_timeout_ticks = 300,
                            size_t timer_wheel_slots = 1024);

    ~SessionManager();

    // 복사/이동 불가
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    /// accept된 소켓으로 새 세션 생성.
    /// @return 새 세션의 shared_ptr (null 아님)
    [[nodiscard]] SessionPtr create_session(
        boost::asio::ip::tcp::socket socket);

    /// id로 세션 제거.
    /// 아직 열려 있으면 세션을 닫는다.
    void remove_session(SessionId id);

    /// id로 세션 검색.
    /// @return 없으면 nullptr
    [[nodiscard]] SessionPtr find_session(SessionId id) const;

    /// 세션의 하트비트 타이머 리셋 (메시지 수신 시마다 호출).
    void touch_session(SessionId id);

    /// 타이밍 휠을 1틱 전진.
    /// 주기적으로 호출할 것 (예: 100ms마다).
    /// 만료된 세션은 닫히고 제거됨.
    void tick();

    /// 세션 타임아웃 이벤트 콜백 설정.
    using TimeoutCallback = std::function<void(SessionPtr)>;
    void set_timeout_callback(TimeoutCallback cb);

    /// 활성 세션 수.
    [[nodiscard]] size_t session_count() const noexcept;

    /// 코어 ID.
    [[nodiscard]] uint32_t core_id() const noexcept { return core_id_; }

private:
    void on_timer_expire(TimingWheel::EntryId entry_id);

    uint32_t core_id_;
    uint32_t heartbeat_timeout_ticks_;
    SessionId next_id_{1};

    std::unordered_map<SessionId, SessionPtr> sessions_;
    TimingWheel timer_wheel_;

    // TimingWheel::EntryId <-> SessionId 양방향 매핑
    std::unordered_map<TimingWheel::EntryId, SessionId> timer_to_session_;
    std::unordered_map<SessionId, TimingWheel::EntryId> session_to_timer_;

    TimeoutCallback timeout_callback_;
};

} // namespace apex::core
```

### Step 5: FlatBuffers 스키마 파일 생성

```
// core/schemas/heartbeat.fbs
namespace apex.messages;

table Heartbeat {
    timestamp_ms:uint64;
}

root_type Heartbeat;
```

```
// core/schemas/error_response.fbs
namespace apex.messages;

table ErrorResponse {
    code:uint16;
    message:string;
}

root_type ErrorResponse;
```

```
// core/schemas/echo.fbs
namespace apex.messages;

table EchoRequest {
    data:[ubyte];
}

table EchoResponse {
    data:[ubyte];
}

root_type EchoRequest;
```

### Step 6: 빈 소스 스텁 생성

```cpp
// core/src/session.cpp
#include <apex/core/session.hpp>

namespace apex::core {
// 구현은 Task 3 (Agent B)에서
} // namespace apex::core
```

```cpp
// core/src/session_manager.cpp
#include <apex/core/session_manager.hpp>

namespace apex::core {
// 구현은 Task 3 (Agent B)에서
} // namespace apex::core
```

### Step 7: core/CMakeLists.txt 업데이트

기존 내용 뒤에 FlatBuffers 코드젠과 새 소스 파일 추가:

```cmake
# find_package(Boost REQUIRED) 뒤에 추가:
find_package(flatbuffers CONFIG REQUIRED)

# FlatBuffers 스키마 컴파일
set(APEX_SCHEMAS
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/heartbeat.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/error_response.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/echo.fbs
)

set(APEX_FBS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${APEX_FBS_GENERATED_DIR})

foreach(SCHEMA ${APEX_SCHEMAS})
    get_filename_component(SCHEMA_NAME ${SCHEMA} NAME_WE)
    set(GENERATED_HEADER ${APEX_FBS_GENERATED_DIR}/${SCHEMA_NAME}_generated.h)
    add_custom_command(
        OUTPUT ${GENERATED_HEADER}
        COMMAND flatbuffers::flatc --cpp -o ${APEX_FBS_GENERATED_DIR} ${SCHEMA}
        DEPENDS ${SCHEMA}
        COMMENT "FlatBuffers 스키마 컴파일: ${SCHEMA_NAME}.fbs"
    )
    list(APPEND APEX_FBS_GENERATED_HEADERS ${GENERATED_HEADER})
endforeach()

add_custom_target(apex_fbs_generate DEPENDS ${APEX_FBS_GENERATED_HEADERS})

# 라이브러리에 새 소스 파일 추가
add_library(apex_core STATIC
    src/slab_pool.cpp
    src/ring_buffer.cpp
    src/timing_wheel.cpp
    src/core_engine.cpp
    src/message_dispatcher.cpp
    src/wire_header.cpp
    src/frame_codec.cpp
    src/session.cpp
    src/session_manager.cpp
)

add_dependencies(apex_core apex_fbs_generate)

# 생성된 헤더를 인클루드 경로에 추가
target_include_directories(apex_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${APEX_FBS_GENERATED_DIR}/..>
        $<INSTALL_INTERFACE:include>
)

# FlatBuffers 링크
target_link_libraries(apex_core
    PUBLIC
        Boost::boost
        flatbuffers::flatbuffers
)
```

**참고:** 생성된 헤더 경로가 `${APEX_FBS_GENERATED_DIR}/..`이므로 `#include "generated/echo_generated.h"`로 인클루드 가능.

### Step 8: 주석 처리된 테스트 스텁 생성

```cpp
// core/tests/unit/test_session.cpp
#include <gtest/gtest.h>
// 테스트는 Task 3 (Agent B)에서 구현
// TEST(Session, ...) {}
```

```cpp
// core/tests/unit/test_session_manager.cpp
#include <gtest/gtest.h>
// 테스트는 Task 3 (Agent B)에서 구현
// TEST(SessionManager, ...) {}
```

```cpp
// core/tests/unit/test_error_propagation.cpp
#include <gtest/gtest.h>
// 테스트는 Task 4 (Agent C)에서 구현
// TEST(ErrorPropagation, ...) {}
```

```cpp
// core/tests/unit/test_flatbuffers_dispatch.cpp
#include <gtest/gtest.h>
// 테스트는 Task 2 (Agent A)에서 구현
// TEST(FlatBuffersDispatch, ...) {}
```

`core/tests/unit/CMakeLists.txt`에 추가:

```cmake
# 기존 테스트 뒤에 추가:
apex_add_unit_test(test_session test_session.cpp)
apex_add_unit_test(test_session_manager test_session_manager.cpp)
apex_add_unit_test(test_error_propagation test_error_propagation.cpp)
apex_add_unit_test(test_flatbuffers_dispatch test_flatbuffers_dispatch.cpp)
```

### Step 9: 스텁 빌드 확인

실행: `build.bat debug`
기대: 기존 11개 + 새 4개 = 15개 테스트 전부 통과 (새 테스트는 케이스 없으니 vacuous pass)

### Step 10: 커밋

```bash
git add -A
git commit -m "chore: Phase 4 헤더 인터페이스 + CMake 스텁

- error_code.hpp, result.hpp (에러 전파 타입) 추가
- session.hpp, session_manager.hpp (세션 관리 API) 추가
- FlatBuffers 스키마 (heartbeat, error_response, echo) 추가
- CMake flatc 코드젠 파이프라인 추가
- 빈 소스 스텁 + 주석 처리된 테스트 파일 추가"
```

---

## Task 2: FlatBuffers 코드젠 + route<T> 타입 디스패치 (Agent A)

**목적:** FlatBuffers 스키마 컴파일 파이프라인 + ServiceBase에서 타입 안전 메시지 디스패치를 위한 `route<T>()` 구현.

**파일:**
- 수정: `core/include/apex/core/service_base.hpp` (route<T> 추가)
- 수정: `core/tests/unit/test_flatbuffers_dispatch.cpp` (테스트 구현)
- 참고: `core/schemas/*.fbs` (생성된 헤더 확인)

**선행 조건:** Task 1 완료 (FlatBuffers 코드젠 동작, 스키마 컴파일됨)

### Step 1: route<T> 디스패치 실패 테스트 작성

```cpp
// core/tests/unit/test_flatbuffers_dispatch.cpp
#include <apex/core/service_base.hpp>
#include <apex/core/wire_header.hpp>
#include <apex/core/frame_codec.hpp>
#include <generated/echo_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

using namespace apex::core;

// 헬퍼: FlatBuffers EchoRequest 페이로드 빌드
static std::vector<uint8_t> build_echo_payload(const std::vector<uint8_t>& data) {
    flatbuffers::FlatBufferBuilder builder(256);
    auto data_vec = builder.CreateVector(data);
    auto req = apex::messages::CreateEchoRequest(builder, data_vec);
    builder.Finish(req);
    return {builder.GetBufferPointer(),
            builder.GetBufferPointer() + builder.GetSize()};
}

// route<T>를 사용하는 테스트 서비스
class TypedEchoService : public ServiceBase<TypedEchoService> {
public:
    TypedEchoService() : ServiceBase("typed_echo") {}

    void on_start() override {
        route<apex::messages::EchoRequest>(
            0x0010, &TypedEchoService::on_echo);
    }

    void on_echo(uint16_t msg_id,
                 const apex::messages::EchoRequest* req) {
        ++call_count;
        if (req && req->data()) {
            last_data.assign(req->data()->begin(), req->data()->end());
        }
    }

    int call_count{0};
    std::vector<uint8_t> last_data;
};

TEST(FlatBuffersDispatch, RouteTypedMessage) {
    auto svc = std::make_unique<TypedEchoService>();
    svc->start();

    auto payload = build_echo_payload({0xDE, 0xAD});
    (void)svc->dispatcher().dispatch(0x0010, payload);

    EXPECT_EQ(svc->call_count, 1);
    EXPECT_EQ(svc->last_data, (std::vector<uint8_t>{0xDE, 0xAD}));
}

TEST(FlatBuffersDispatch, RouteInvalidFlatBuffer) {
    auto svc = std::make_unique<TypedEchoService>();
    svc->start();

    // 쓰레기 페이로드 전송 — 크래시 없이 핸들러 호출 안 됨
    std::vector<uint8_t> garbage = {0xFF, 0xFF, 0xFF};
    (void)svc->dispatcher().dispatch(0x0010, garbage);

    // 잘못된 데이터로는 핸들러가 호출되지 않아야 함
    EXPECT_EQ(svc->call_count, 0);
}

TEST(FlatBuffersDispatch, RouteAndRawHandlerCoexist) {
    auto svc = std::make_unique<TypedEchoService>();
    svc->start();

    int raw_count = 0;
    // 타입 route와 로우 핸들러 공존 테스트
    svc->dispatcher().register_handler(0x0020,
        [&](uint16_t, std::span<const uint8_t>) { ++raw_count; });

    auto payload = build_echo_payload({0x42});
    (void)svc->dispatcher().dispatch(0x0010, payload);
    (void)svc->dispatcher().dispatch(0x0020, {});

    EXPECT_EQ(svc->call_count, 1);
    EXPECT_EQ(raw_count, 1);
}
```

### Step 2: 테스트 실패 확인

실행: `build.bat debug`
기대: FAIL — `route<T>`가 ServiceBase에 정의되지 않음

### Step 3: ServiceBase에 route<T> 구현

`core/include/apex/core/service_base.hpp` 상단에 인클루드 추가:

```cpp
#include <flatbuffers/flatbuffers.h>
```

ServiceBase 클래스 내부, handle() 뒤에 추가:

```cpp
    /// FlatBuffers 타입 메시지 핸들러 등록.
    /// 핸들러 호출 전 FlatBuffers 버퍼를 자동 검증한다.
    /// 검증 실패 시 핸들러 호출하지 않고 메시지를 무시한다.
    ///
    /// 사용법:
    ///   route<MyMessage>(0x0010, &MyService::on_my_message);
    ///
    /// 핸들러 시그니처:
    ///   void on_my_message(uint16_t msg_id, const MyMessage* msg);
    template <typename FbsType>
    void route(uint16_t msg_id,
               void (Derived::*method)(uint16_t, const FbsType*))
    {
        auto* self = static_cast<Derived*>(this);
        dispatcher_.register_handler(msg_id,
            [self, method](uint16_t id, std::span<const uint8_t> payload) {
                // FlatBuffers 무결성 검증
                flatbuffers::Verifier verifier(payload.data(), payload.size());
                if (!verifier.VerifyBuffer<FbsType>()) {
                    return;  // 잘못된 메시지 무시
                }
                auto* msg = flatbuffers::GetRoot<FbsType>(payload.data());
                (self->*method)(id, msg);
            });
    }
```

### Step 4: 테스트 통과 확인

실행: `build.bat debug`
기대: 모든 FlatBuffersDispatch 테스트 PASS

### Step 5: 커밋

```bash
git add core/include/apex/core/service_base.hpp \
        core/tests/unit/test_flatbuffers_dispatch.cpp
git commit -m "feat: ServiceBase에 route<T> FlatBuffers 타입 디스패치 추가

- route<T>()가 핸들러 호출 전 FlatBuffers 버퍼 자동 검증
- 잘못된 버퍼는 크래시 없이 무시
- 기존 handle() 로우 핸들러와 공존 가능"
```

---

## Task 3: 세션 관리 (Agent B)

**목적:** TCP 연결 + RingBuffer를 감싸는 Session 클래스, 코어별 SessionManager와 TimingWheel 하트비트 타임아웃 연동.

**파일:**
- 수정: `core/src/session.cpp` (Session 구현)
- 수정: `core/src/session_manager.cpp` (SessionManager 구현)
- 수정: `core/tests/unit/test_session.cpp` (테스트 구현)
- 수정: `core/tests/unit/test_session_manager.cpp` (테스트 구현)

**선행 조건:** Task 1 완료

### Step 1: Session 실패 테스트 작성

```cpp
// core/tests/unit/test_session.cpp
#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>

#include <gtest/gtest.h>

using namespace apex::core;
using boost::asio::ip::tcp;

class SessionTest : public ::testing::Test {
protected:
    boost::asio::io_context io_ctx_;

    // 테스트용 연결된 소켓 쌍 생성
    std::pair<tcp::socket, tcp::socket> make_socket_pair() {
        tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
        auto port = acceptor.local_endpoint().port();

        tcp::socket client(io_ctx_);
        client.connect(tcp::endpoint(
            boost::asio::ip::address_v4::loopback(), port));
        auto server = acceptor.accept();
        return {std::move(server), std::move(client)};
    }
};

TEST_F(SessionTest, InitialState) {
    auto [server, client] = make_socket_pair();
    Session session(1, std::move(server), 0);

    EXPECT_EQ(session.id(), 1u);
    EXPECT_EQ(session.core_id(), 0u);
    EXPECT_EQ(session.state(), Session::State::Connected);
    EXPECT_TRUE(session.is_open());

    client.close();
}

TEST_F(SessionTest, SendFrame) {
    auto [server, client] = make_socket_pair();
    auto session = std::make_shared<Session>(1, std::move(server), 0);

    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    WireHeader header{.msg_id = 0x0042,
                      .body_size = static_cast<uint32_t>(payload.size())};
    EXPECT_TRUE(session->send(header, payload));

    // 클라이언트 측에서 응답 읽기
    std::vector<uint8_t> response(WireHeader::SIZE + payload.size());
    boost::asio::read(client, boost::asio::buffer(response));

    auto parsed = WireHeader::parse(response);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->msg_id, 0x0042);
    EXPECT_EQ(parsed->body_size, 4u);

    client.close();
}

TEST_F(SessionTest, SendAfterClose) {
    auto [server, client] = make_socket_pair();
    auto session = std::make_shared<Session>(1, std::move(server), 0);

    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);
    EXPECT_FALSE(session->is_open());

    std::vector<uint8_t> payload = {0x01};
    WireHeader header{.msg_id = 1, .body_size = 1};
    EXPECT_FALSE(session->send(header, payload));

    client.close();
}

TEST_F(SessionTest, RecvBufferAccessible) {
    auto [server, client] = make_socket_pair();
    Session session(1, std::move(server), 0, 4096);

    EXPECT_EQ(session.recv_buffer().capacity(), 4096u);
    EXPECT_EQ(session.recv_buffer().readable_size(), 0u);

    client.close();
}
```

### Step 2: 테스트 실패 확인

실행: `build.bat debug`
기대: FAIL — Session 메서드가 구현되지 않음

### Step 3: Session 구현

```cpp
// core/src/session.cpp
#include <apex/core/session.hpp>

#include <boost/asio/write.hpp>

#include <cstring>

namespace apex::core {

Session::Session(SessionId id, boost::asio::ip::tcp::socket socket,
                 uint32_t core_id, size_t recv_buf_capacity)
    : id_(id)
    , core_id_(core_id)
    , socket_(std::move(socket))
    , recv_buf_(recv_buf_capacity)
{
}

Session::~Session() {
    if (socket_.is_open()) {
        boost::system::error_code ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
}

bool Session::send(const WireHeader& header,
                   std::span<const uint8_t> payload) {
    if (!is_open()) return false;

    std::vector<uint8_t> frame(header.frame_size());
    auto hdr_bytes = header.serialize();
    std::memcpy(frame.data(), hdr_bytes.data(), WireHeader::SIZE);
    if (!payload.empty()) {
        std::memcpy(frame.data() + WireHeader::SIZE,
                    payload.data(), payload.size());
    }

    boost::system::error_code ec;
    boost::asio::write(socket_, boost::asio::buffer(frame), ec);
    if (ec) {
        close();
        return false;
    }
    return true;
}

bool Session::send_raw(std::span<const uint8_t> data) {
    if (!is_open()) return false;

    boost::system::error_code ec;
    boost::asio::write(socket_, boost::asio::buffer(data.data(), data.size()), ec);
    if (ec) {
        close();
        return false;
    }
    return true;
}

void Session::close() {
    if (state_ == State::Closed) return;
    state_ = State::Closed;

    boost::system::error_code ec;
    if (socket_.is_open()) {
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
}

} // namespace apex::core
```

### Step 4: Session 테스트 실행

실행: `build.bat debug`
기대: 모든 Session 테스트 PASS

### Step 5: SessionManager 실패 테스트 작성

```cpp
// core/tests/unit/test_session_manager.cpp
#include <apex/core/session_manager.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <gtest/gtest.h>

using namespace apex::core;
using boost::asio::ip::tcp;

class SessionManagerTest : public ::testing::Test {
protected:
    boost::asio::io_context io_ctx_;

    std::pair<tcp::socket, tcp::socket> make_socket_pair() {
        tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
        auto port = acceptor.local_endpoint().port();
        tcp::socket client(io_ctx_);
        client.connect(tcp::endpoint(
            boost::asio::ip::address_v4::loopback(), port));
        auto server = acceptor.accept();
        return {std::move(server), std::move(client)};
    }
};

TEST_F(SessionManagerTest, CreateAndFindSession) {
    SessionManager mgr(0, 300, 64);
    auto [server, client] = make_socket_pair();

    auto session = mgr.create_session(std::move(server));
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->core_id(), 0u);
    EXPECT_TRUE(session->is_open());
    EXPECT_EQ(mgr.session_count(), 1u);

    auto found = mgr.find_session(session->id());
    EXPECT_EQ(found, session);

    client.close();
}

TEST_F(SessionManagerTest, RemoveSession) {
    SessionManager mgr(0, 300, 64);
    auto [server, client] = make_socket_pair();

    auto session = mgr.create_session(std::move(server));
    auto id = session->id();
    mgr.remove_session(id);

    EXPECT_EQ(mgr.session_count(), 0u);
    EXPECT_EQ(mgr.find_session(id), nullptr);

    client.close();
}

TEST_F(SessionManagerTest, HeartbeatTimeout) {
    // 3틱 타임아웃, 8슬롯
    SessionManager mgr(0, 3, 8);
    auto [server, client] = make_socket_pair();

    SessionPtr timed_out_session;
    mgr.set_timeout_callback([&](SessionPtr s) {
        timed_out_session = s;
    });

    auto session = mgr.create_session(std::move(server));
    auto id = session->id();

    // 3번 틱 — 타임아웃 발생해야 함
    mgr.tick();
    mgr.tick();
    mgr.tick();

    EXPECT_NE(timed_out_session, nullptr);
    EXPECT_EQ(timed_out_session->id(), id);
    EXPECT_EQ(mgr.session_count(), 0u);

    client.close();
}

TEST_F(SessionManagerTest, TouchResetsTimeout) {
    SessionManager mgr(0, 3, 8);
    auto [server, client] = make_socket_pair();

    SessionPtr timed_out_session;
    mgr.set_timeout_callback([&](SessionPtr s) {
        timed_out_session = s;
    });

    auto session = mgr.create_session(std::move(server));

    // 2번 틱 후 touch, 그 다음 2번 더 틱
    mgr.tick();
    mgr.tick();
    mgr.touch_session(session->id());
    mgr.tick();
    mgr.tick();

    // 아직 타임아웃 안 됐어야 함 (touch가 타이머 리셋)
    EXPECT_EQ(timed_out_session, nullptr);
    EXPECT_EQ(mgr.session_count(), 1u);

    // 한 번 더 틱하면 타임아웃
    mgr.tick();
    EXPECT_NE(timed_out_session, nullptr);

    client.close();
}

TEST_F(SessionManagerTest, MultipleSessions) {
    SessionManager mgr(0, 300, 64);
    std::vector<tcp::socket> clients;

    for (int i = 0; i < 5; ++i) {
        auto [server, client] = make_socket_pair();
        mgr.create_session(std::move(server));
        clients.push_back(std::move(client));
    }

    EXPECT_EQ(mgr.session_count(), 5u);

    for (auto& c : clients) c.close();
}

TEST_F(SessionManagerTest, DisabledHeartbeat) {
    // heartbeat_timeout_ticks = 0이면 비활성화
    SessionManager mgr(0, 0, 8);
    auto [server, client] = make_socket_pair();

    SessionPtr timed_out_session;
    mgr.set_timeout_callback([&](SessionPtr s) {
        timed_out_session = s;
    });

    auto session = mgr.create_session(std::move(server));

    // 많이 틱해도 타임아웃 안 됨
    for (int i = 0; i < 100; ++i) mgr.tick();

    EXPECT_EQ(timed_out_session, nullptr);
    EXPECT_EQ(mgr.session_count(), 1u);

    client.close();
}
```

### Step 6: 테스트 실패 확인

실행: `build.bat debug`
기대: FAIL — SessionManager 메서드가 구현되지 않음

### Step 7: SessionManager 구현

```cpp
// core/src/session_manager.cpp
#include <apex/core/session_manager.hpp>

namespace apex::core {

SessionManager::SessionManager(uint32_t core_id,
                               uint32_t heartbeat_timeout_ticks,
                               size_t timer_wheel_slots)
    : core_id_(core_id)
    , heartbeat_timeout_ticks_(heartbeat_timeout_ticks)
    , timer_wheel_(timer_wheel_slots, [this](TimingWheel::EntryId entry_id) {
          on_timer_expire(entry_id);
      })
{
}

SessionManager::~SessionManager() = default;

SessionPtr SessionManager::create_session(
    boost::asio::ip::tcp::socket socket)
{
    SessionId id = next_id_++;
    auto session = std::make_shared<Session>(
        id, std::move(socket), core_id_);
    session->set_state(Session::State::Active);

    sessions_[id] = session;

    // 하트비트 활성화 시 타임아웃 스케줄
    if (heartbeat_timeout_ticks_ > 0) {
        auto timer_id = timer_wheel_.schedule(heartbeat_timeout_ticks_);
        timer_to_session_[timer_id] = id;
        session_to_timer_[id] = timer_id;
    }

    return session;
}

void SessionManager::remove_session(SessionId id) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;

    auto& session = it->second;
    session->close();

    // 하트비트 타이머 취소
    auto timer_it = session_to_timer_.find(id);
    if (timer_it != session_to_timer_.end()) {
        timer_wheel_.cancel(timer_it->second);
        timer_to_session_.erase(timer_it->second);
        session_to_timer_.erase(timer_it);
    }

    sessions_.erase(it);
}

SessionPtr SessionManager::find_session(SessionId id) const {
    auto it = sessions_.find(id);
    return (it != sessions_.end()) ? it->second : nullptr;
}

void SessionManager::touch_session(SessionId id) {
    if (heartbeat_timeout_ticks_ == 0) return;

    auto timer_it = session_to_timer_.find(id);
    if (timer_it == session_to_timer_.end()) return;

    // 기존 타이머 취소 후 새로 스케줄
    auto old_timer = timer_it->second;
    timer_to_session_.erase(old_timer);
    timer_wheel_.cancel(old_timer);

    auto new_timer = timer_wheel_.schedule(heartbeat_timeout_ticks_);
    timer_to_session_[new_timer] = id;
    timer_it->second = new_timer;
}

void SessionManager::tick() {
    timer_wheel_.tick();
}

void SessionManager::set_timeout_callback(TimeoutCallback cb) {
    timeout_callback_ = std::move(cb);
}

size_t SessionManager::session_count() const noexcept {
    return sessions_.size();
}

void SessionManager::on_timer_expire(TimingWheel::EntryId entry_id) {
    auto it = timer_to_session_.find(entry_id);
    if (it == timer_to_session_.end()) return;

    SessionId session_id = it->second;
    timer_to_session_.erase(it);
    session_to_timer_.erase(session_id);

    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) return;

    auto session = session_it->second;

    if (timeout_callback_) {
        timeout_callback_(session);
    }

    session->close();
    sessions_.erase(session_it);
}

} // namespace apex::core
```

### Step 8: 전체 테스트 실행

실행: `build.bat debug`
기대: 모든 Session + SessionManager 테스트 PASS

### Step 9: 커밋

```bash
git add core/src/session.cpp core/src/session_manager.cpp \
        core/include/apex/core/session_manager.hpp \
        core/tests/unit/test_session.cpp \
        core/tests/unit/test_session_manager.cpp
git commit -m "feat: Session + SessionManager (하트비트 타임아웃 연동)

- Session: TCP 소켓 + RingBuffer 래핑, send/close API
- SessionManager: 코어별 세션 관리 (create, find, remove)
- TimingWheel 연동 하트비트 타임아웃
- touch_session()으로 활동 시 타임아웃 리셋
- 타임아웃 콜백으로 커스텀 연결 끊김 처리"
```

---

## Task 4: 에러 전파 (Agent C)

**목적:** 디스패치 실패나 핸들러 에러 시 자동 ErrorResponse 생성. wire_flags::ERROR_RESPONSE와 ErrorResponse FlatBuffers 스키마 활용.

**파일:**
- 수정: `core/tests/unit/test_error_propagation.cpp` (테스트 구현)
- 생성: `core/include/apex/core/error_sender.hpp` (에러 프레임 빌더)
- 생성: `core/src/error_sender.cpp`
- 수정: `core/CMakeLists.txt` (error_sender.cpp 추가)

**선행 조건:** Task 1 완료 (error_code.hpp, result.hpp, ErrorResponse 스키마)

### Step 1: 실패 테스트 작성

```cpp
// core/tests/unit/test_error_propagation.cpp
#include <apex/core/error_code.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/result.hpp>
#include <apex/core/wire_header.hpp>

#include <generated/error_response_generated.h>
#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

using namespace apex::core;

TEST(Result, OkValue) {
    Result<int> r = 42;
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 42);
}

TEST(Result, ErrorValue) {
    Result<int> r = error(ErrorCode::Timeout);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ErrorCode::Timeout);
}

TEST(Result, VoidOk) {
    Result<void> r = ok();
    EXPECT_TRUE(r.has_value());
}

TEST(Result, VoidError) {
    Result<void> r = error(ErrorCode::SessionClosed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ErrorCode::SessionClosed);
}

TEST(ErrorCode, NameLookup) {
    EXPECT_EQ(error_code_name(ErrorCode::Ok), "Ok");
    EXPECT_EQ(error_code_name(ErrorCode::Timeout), "Timeout");
    EXPECT_EQ(error_code_name(ErrorCode::HandlerNotFound), "HandlerNotFound");
}

TEST(ErrorSender, BuildErrorFrame) {
    auto frame = ErrorSender::build_error_frame(
        0x0042, ErrorCode::Timeout, "request timed out");

    ASSERT_GT(frame.size(), WireHeader::SIZE);

    // 헤더 파싱
    auto header = WireHeader::parse(frame);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->msg_id, 0x0042);
    EXPECT_TRUE(header->flags & wire_flags::ERROR_RESPONSE);

    // FlatBuffers 페이로드 파싱
    auto payload = std::span<const uint8_t>(
        frame.data() + WireHeader::SIZE, header->body_size);
    auto resp = flatbuffers::GetRoot<apex::messages::ErrorResponse>(
        payload.data());
    EXPECT_EQ(resp->code(), static_cast<uint16_t>(ErrorCode::Timeout));
    EXPECT_STREQ(resp->message()->c_str(), "request timed out");
}

TEST(ErrorSender, BuildErrorFrameNoMessage) {
    auto frame = ErrorSender::build_error_frame(
        0x0001, ErrorCode::HandlerNotFound);

    auto header = WireHeader::parse(frame);
    ASSERT_TRUE(header.has_value());
    EXPECT_TRUE(header->flags & wire_flags::ERROR_RESPONSE);

    auto payload = std::span<const uint8_t>(
        frame.data() + WireHeader::SIZE, header->body_size);
    auto resp = flatbuffers::GetRoot<apex::messages::ErrorResponse>(
        payload.data());
    EXPECT_EQ(resp->code(),
              static_cast<uint16_t>(ErrorCode::HandlerNotFound));
}
```

### Step 2: 테스트 실패 확인

실행: `build.bat debug`
기대: FAIL — ErrorSender가 정의되지 않음

### Step 3: error_sender.hpp 생성

```cpp
// core/include/apex/core/error_sender.hpp
#pragma once

#include <apex/core/error_code.hpp>
#include <apex/core/wire_header.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace apex::core {

/// 와이어 포맷 에러 응답 프레임 빌더.
/// FlatBuffers ErrorResponse 스키마 + wire_flags::ERROR_RESPONSE 사용.
class ErrorSender {
public:
    /// 완전한 에러 프레임 빌드 (헤더 + FlatBuffers ErrorResponse 페이로드).
    /// @param original_msg_id 에러를 발생시킨 요청의 msg_id
    /// @param code 에러 코드
    /// @param message 선택적 사람이 읽을 수 있는 에러 메시지
    /// @return Session::send_raw()로 바로 전송 가능한 완전한 프레임
    [[nodiscard]] static std::vector<uint8_t> build_error_frame(
        uint16_t original_msg_id,
        ErrorCode code,
        std::string_view message = "");
};

} // namespace apex::core
```

### Step 4: error_sender.cpp 구현

```cpp
// core/src/error_sender.cpp
#include <apex/core/error_sender.hpp>

#include <generated/error_response_generated.h>
#include <flatbuffers/flatbuffers.h>

#include <cstring>

namespace apex::core {

std::vector<uint8_t> ErrorSender::build_error_frame(
    uint16_t original_msg_id,
    ErrorCode code,
    std::string_view message)
{
    // FlatBuffers 페이로드 빌드
    flatbuffers::FlatBufferBuilder builder(128);

    flatbuffers::Offset<flatbuffers::String> msg_offset;
    if (!message.empty()) {
        msg_offset = builder.CreateString(message.data(), message.size());
    }

    auto resp = apex::messages::CreateErrorResponse(
        builder,
        static_cast<uint16_t>(code),
        msg_offset);
    builder.Finish(resp);

    auto payload_data = builder.GetBufferPointer();
    auto payload_size = builder.GetSize();

    // 와이어 헤더 빌드
    WireHeader header{
        .msg_id = original_msg_id,
        .body_size = static_cast<uint32_t>(payload_size),
        .flags = wire_flags::ERROR_RESPONSE,
    };

    // 완전한 프레임 조립
    std::vector<uint8_t> frame(header.frame_size());
    auto hdr_bytes = header.serialize();
    std::memcpy(frame.data(), hdr_bytes.data(), WireHeader::SIZE);
    std::memcpy(frame.data() + WireHeader::SIZE, payload_data, payload_size);

    return frame;
}

} // namespace apex::core
```

### Step 5: CMakeLists.txt에 error_sender.cpp 추가

`add_library(apex_core STATIC ...)` 목록에 `src/error_sender.cpp` 추가.

### Step 6: 테스트 실행

실행: `build.bat debug`
기대: 모든 ErrorPropagation 테스트 PASS

### Step 7: 커밋

```bash
git add core/include/apex/core/error_sender.hpp \
        core/include/apex/core/error_code.hpp \
        core/include/apex/core/result.hpp \
        core/src/error_sender.cpp \
        core/CMakeLists.txt \
        core/tests/unit/test_error_propagation.cpp
git commit -m "feat: ErrorSender + Result<T> 에러 전파

- ErrorCode 열거형 (프레임워크 + 앱 에러)
- Result<T> = std::expected<T, ErrorCode>, ok()/error() 헬퍼
- ErrorSender: 와이어 포맷 에러 프레임 빌드 (FlatBuffers ErrorResponse)
- wire_flags::ERROR_RESPONSE 플래그 설정
- error_code_name()으로 사람이 읽을 수 있는 에러명 반환"
```

---

## Task 5: 빌드 검증 + 통합 연결 (순차)

**목적:** 모든 병렬 태스크가 함께 컴파일되는지 확인. 통합 문제 수정.

**파일:**
- 확인: Task 2-4에서 수정된 모든 파일
- 필요 시 수정: CMakeLists.txt, 인클루드, 링크 에러

### Step 1: 클린 빌드

`build/debug` 삭제 후 처음부터 다시 빌드:

```bash
rmdir /s /q build\debug
build.bat debug
```

기대: 에러 0개, 경고 0개, 15개 이상 테스트 전부 통과

### Step 2: 테스트 목록 확인

실행: `ctest --test-dir build/debug -N` (테스트 실행 없이 목록만)

기대하는 테스트 목록:
- test_mpsc_queue (기존)
- test_slab_pool (기존)
- test_ring_buffer (기존)
- test_timing_wheel (기존)
- test_core_engine (기존)
- test_message_dispatcher (기존)
- test_service_base (기존)
- test_wire_header (기존)
- test_frame_codec (기존)
- test_pipeline_integration (기존)
- test_echo_integration (기존)
- test_session (신규)
- test_session_manager (신규)
- test_error_propagation (신규)
- test_flatbuffers_dispatch (신규)

### Step 3: 수정 필요 시 커밋

```bash
git add -A
git commit -m "fix: Phase 4 통합 연결 수정"
```

---

## 요약

| Task | 컴포넌트 | 에이전트 | 의존성 | 예상 테스트 수 |
|------|----------|----------|--------|---------------|
| 1 | 헤더 스텁 + CMake | 순차 | 없음 | 0 (스텁만) |
| 2 | FlatBuffers + route<T> | Agent A | Task 1 | 3 |
| 3 | Session + SessionManager | Agent B | Task 1 | 10 |
| 4 | 에러 전파 | Agent C | Task 1 | 7 |
| 5 | 빌드 검증 | 순차 | Task 2-4 | 0 (검증만) |

**병렬화:** Task 2, 3, 4는 Task 1 완료 후 동시 실행 가능.

**신규 테스트 합계:** ~20개
**Phase 4 이후 전체 테스트:** ~31개 (단위 20 + 통합 2 + 기존 9)

**다음 Phase (4.5):** 통합 테스트 + 채팅 예제 → v0.2.0
