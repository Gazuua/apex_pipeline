# SocketBase Virtual Interface 도입 — BACKLOG-133 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** TransportContext에서 `ssl::context*`를 제거하고, Session이 `unique_ptr<SocketBase>`를 소유하도록 리팩터링하여 apex_core의 OpenSSL 직접 의존을 해소한다.

**Architecture:** SocketBase virtual interface(async_read_some, async_write, close, is_open)를 도입하고, TcpSocket/TlsSocket 구현체가 이를 상속한다. Session/SessionManager/ConnectionHandler는 SocketBase 포인터를 통해 소켓에 접근하므로 비템플릿 상태를 유지한다. ssl::context 소유권은 Listener<P, TlsTcpTransport> 레벨로 이동한다.

**Tech Stack:** C++23, Boost.Asio (awaitable, ssl), MSVC/GCC/Clang, GTest

**설계 출처:** `docs/apex_common/plans/20260322_162133_fsd_design_decisions_batch.md` §133 — B안 채택

---

## 파일 구조

### 신규 파일

| 파일 | 책임 |
|------|------|
| `apex_core/include/apex/core/socket_base.hpp` | SocketBase virtual interface + TcpSocket 구현체 |
| `apex_core/tests/unit/test_socket_base.cpp` | SocketBase/TcpSocket 단위 테스트 |

### 수정 파일

| 파일 | 변경 내용 |
|------|-----------|
| `apex_core/include/apex/core/session.hpp` | `tcp::socket socket_` → `unique_ptr<SocketBase> socket_`, 생성자 시그니처, `socket()` 접근자 |
| `apex_core/src/session.cpp` | 생성자, close(), write_pump()에서 SocketBase 사용 |
| `apex_core/include/apex/core/session_manager.hpp` | `create_session(tcp::socket)` → `create_session(unique_ptr<SocketBase>)` |
| `apex_core/src/session_manager.cpp` | create_session 구현 변경 |
| `apex_core/include/apex/core/connection_handler.hpp` | `accept_connection` 시그니처, read_loop에서 SocketBase 사용 |
| `apex_core/include/apex/core/listener.hpp` | accept 콜백에서 TcpSocket wrap, TLS 변형 대비 |
| `apex_core/include/apex/core/transport.hpp` | TransportContext에서 `ssl_ctx` 제거, `ssl/context.hpp` include 제거 |
| `apex_shared/lib/protocols/tcp/include/apex/shared/protocols/tcp/tls_tcp_transport.hpp` | make_socket 시그니처 변경 (TransportContext 대신 ssl::context& 직접 수신) |
| `apex_core/tests/test_helpers.hpp` | `make_socket_pair` → SocketBase 버전 추가 |
| `apex_core/tests/unit/test_session_manager.cpp` | TcpSocket wrap 적용 |
| `apex_core/CMakeLists.txt` | test_socket_base.cpp 추가 |

기존 integration 테스트(`test_server_e2e.cpp`, `test_echo_integration.cpp` 등)는 Server::listen<P, T>를 경유하므로 Listener 내부에서 자동으로 TcpSocket wrap이 적용되어 변경 불필요.

---

## Task 1: SocketBase Interface + TcpSocket 구현

**Files:**
- Create: `apex_core/include/apex/core/socket_base.hpp`
- Create: `apex_core/tests/unit/test_socket_base.cpp`
- Modify: `apex_core/CMakeLists.txt`

- [ ] **Step 1: SocketBase 인터페이스 + TcpSocket 구현체 작성**

```cpp
// apex_core/include/apex/core/socket_base.hpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <system_error>
#include <utility>

namespace apex::core
{

/// Virtual socket interface — 타입 소거를 통해 Session이 tcp::socket과
/// ssl::stream<tcp::socket>을 동일하게 취급할 수 있도록 한다.
///
/// 설계 근거 (B안):
/// - Session/SessionManager 비템플릿 유지 — 템플릿 전파 방지
/// - I/O virtual dispatch ~2ns는 커널 syscall 대비 무시 가능
/// - 기존 "cold path virtual, hot path template" 패턴과 일관
///
/// @see docs/apex_common/plans/20260322_162133_fsd_design_decisions_batch.md §133
class SocketBase
{
  public:
    virtual ~SocketBase() = default;

    /// 비동기 읽기 — 사용 가능한 데이터를 buf에 읽는다.
    /// @return {error_code, bytes_transferred}
    virtual boost::asio::awaitable<std::pair<boost::system::error_code, size_t>>
    async_read_some(boost::asio::mutable_buffer buf) = 0;

    /// 비동기 쓰기 — buf의 모든 데이터를 전송한다.
    /// @return {error_code, bytes_transferred}
    virtual boost::asio::awaitable<std::pair<boost::system::error_code, size_t>>
    async_write(boost::asio::const_buffer buf) = 0;

    /// 소켓 닫기 (shutdown + close). 에러 무시.
    virtual void close() noexcept = 0;

    /// 소켓이 열려있는지.
    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    /// 소켓의 executor — timer/write_pump co_spawn 시 필요.
    [[nodiscard]] virtual boost::asio::any_io_executor get_executor() noexcept = 0;

    /// TCP 옵션 설정 (tcp_nodelay 등). 하위 레이어 소켓에 적용.
    virtual void set_option_no_delay(bool enabled) = 0;
};

/// Plain TCP 소켓 래퍼.
class TcpSocket final : public SocketBase
{
  public:
    explicit TcpSocket(boost::asio::ip::tcp::socket socket)
        : socket_(std::move(socket))
    {}

    boost::asio::awaitable<std::pair<boost::system::error_code, size_t>>
    async_read_some(boost::asio::mutable_buffer buf) override
    {
        co_return co_await socket_.async_read_some(buf, boost::asio::as_tuple(boost::asio::use_awaitable));
    }

    boost::asio::awaitable<std::pair<boost::system::error_code, size_t>>
    async_write(boost::asio::const_buffer buf) override
    {
        co_return co_await boost::asio::async_write(socket_, buf,
                                                    boost::asio::as_tuple(boost::asio::use_awaitable));
    }

    void close() noexcept override
    {
        boost::system::error_code ec;
        if (socket_.is_open())
        {
            socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        }
    }

    [[nodiscard]] bool is_open() const noexcept override
    {
        return socket_.is_open();
    }

    [[nodiscard]] boost::asio::any_io_executor get_executor() noexcept override
    {
        return socket_.get_executor();
    }

    void set_option_no_delay(bool enabled) override
    {
        boost::system::error_code ec;
        socket_.set_option(boost::asio::ip::tcp::no_delay(enabled), ec);
    }

  private:
    boost::asio::ip::tcp::socket socket_;
};

/// 헬퍼: tcp::socket → unique_ptr<SocketBase> 변환
inline std::unique_ptr<SocketBase> make_tcp_socket(boost::asio::ip::tcp::socket socket)
{
    return std::make_unique<TcpSocket>(std::move(socket));
}

} // namespace apex::core
```

- [ ] **Step 2: TcpSocket 단위 테스트 작성**

```cpp
// apex_core/tests/unit/test_socket_base.cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "../test_helpers.hpp"
#include <apex/core/socket_base.hpp>

#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

using namespace apex::core;

class SocketBaseTest : public ::testing::Test
{
  protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(SocketBaseTest, TcpSocketIsOpenAfterConstruction)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    EXPECT_TRUE(socket->is_open());
    client.close();
}

TEST_F(SocketBaseTest, TcpSocketCloseWorks)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    socket->close();
    EXPECT_FALSE(socket->is_open());
    client.close();
}

TEST_F(SocketBaseTest, TcpSocketCloseIdempotent)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    socket->close();
    socket->close(); // must not crash
    EXPECT_FALSE(socket->is_open());
    client.close();
}

TEST_F(SocketBaseTest, TcpSocketSetNoDelay)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    socket->set_option_no_delay(true); // must not crash
    EXPECT_TRUE(socket->is_open());
    client.close();
}

TEST_F(SocketBaseTest, TcpSocketReadWrite)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    const std::vector<uint8_t> send_data = {0x01, 0x02, 0x03, 0x04};

    apex::test::run_coro(io_ctx_, [&]() -> boost::asio::awaitable<void> {
        // Write from client
        co_await boost::asio::async_write(client, boost::asio::buffer(send_data),
                                          boost::asio::as_tuple(boost::asio::use_awaitable));

        // Read through SocketBase
        std::vector<uint8_t> recv_data(4);
        auto [ec, n] = co_await socket->async_read_some(boost::asio::buffer(recv_data));
        EXPECT_FALSE(ec);
        EXPECT_EQ(n, 4u);
        EXPECT_EQ(recv_data, send_data);

        client.close();
        co_return;
    }());
}

TEST_F(SocketBaseTest, TcpSocketAsyncWrite)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    const std::vector<uint8_t> send_data = {0xAA, 0xBB, 0xCC};

    apex::test::run_coro(io_ctx_, [&]() -> boost::asio::awaitable<void> {
        // Write through SocketBase
        auto [ec, n] = co_await socket->async_write(boost::asio::buffer(send_data));
        EXPECT_FALSE(ec);
        EXPECT_EQ(n, 3u);

        // Read from client
        std::vector<uint8_t> recv_data(3);
        auto [ec2, n2] = co_await client.async_read_some(boost::asio::buffer(recv_data),
                                                          boost::asio::as_tuple(boost::asio::use_awaitable));
        EXPECT_FALSE(ec2);
        EXPECT_EQ(n2, 3u);
        EXPECT_EQ(recv_data, send_data);

        client.close();
        co_return;
    }());
}

TEST_F(SocketBaseTest, TcpSocketGetExecutor)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    auto executor = socket->get_executor();
    // executor must be valid (non-default)
    EXPECT_TRUE(executor != boost::asio::any_io_executor{});
    client.close();
}

TEST_F(SocketBaseTest, MakeTcpSocketFactory)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);

    // make_tcp_socket returns SocketBase*
    std::unique_ptr<SocketBase> socket = make_tcp_socket(std::move(server));
    EXPECT_NE(socket, nullptr);
    EXPECT_TRUE(socket->is_open());
    client.close();
}
```

- [ ] **Step 3: CMakeLists.txt에 테스트 추가**

`apex_core/CMakeLists.txt`의 unit test 리스트에 `tests/unit/test_socket_base.cpp` 추가.

- [ ] **Step 4: 빌드 & 테스트 실행**

Run: `apex-agent queue build debug --target apex_core`
Expected: 빌드 성공, test_socket_base 전체 PASS

- [ ] **Step 5: 커밋**

```
feat(core): SocketBase virtual interface + TcpSocket 구현 (BACKLOG-133)
```

---

## Task 2: Session을 SocketBase 기반으로 전환

**Files:**
- Modify: `apex_core/include/apex/core/session.hpp:58,136-139,168`
- Modify: `apex_core/src/session.cpp:45-53,90-110,167`
- Modify: `apex_core/tests/test_helpers.hpp`

- [ ] **Step 1: Session 헤더 수정**

`session.hpp` 변경사항:

1. `#include <apex/core/socket_base.hpp>` 추가, `#include <boost/asio/ip/tcp.hpp>` 제거
2. 생성자:
```cpp
// 변경 전:
Session(SessionId id, boost::asio::ip::tcp::socket socket, uint32_t core_id, ...);

// 변경 후:
Session(SessionId id, std::unique_ptr<SocketBase> socket, uint32_t core_id, ...);
```

3. `socket()` 접근자:
```cpp
// 변경 전:
[[nodiscard]] boost::asio::ip::tcp::socket& socket() noexcept { return socket_; }

// 변경 후:
[[nodiscard]] SocketBase& socket() noexcept { return *socket_; }
```

4. 멤버 변수:
```cpp
// 변경 전:
boost::asio::ip::tcp::socket socket_;

// 변경 후:
std::unique_ptr<SocketBase> socket_;
```

- [ ] **Step 2: Session 구현 수정**

`session.cpp` 변경사항:

1. 생성자:
```cpp
// 변경 후:
Session::Session(SessionId id, std::unique_ptr<SocketBase> socket, uint32_t core_id,
                 size_t recv_buf_capacity, size_t max_queue_depth)
    : id_(id)
    , core_id_(core_id)
    , socket_(std::move(socket))
    , core_executor_(socket_->get_executor())
    , recv_buf_(recv_buf_capacity)
    , max_queue_depth_(max_queue_depth)
{}
```

2. `close()`:
```cpp
// 변경 후 — socket_ 직접 shutdown/close 대신 SocketBase::close() 위임:
void Session::close() noexcept
{
    auto prev = state_.load(std::memory_order_relaxed);
    if (prev == State::Closed)
        return;
    state_.store(State::Closed, std::memory_order_relaxed);
    s_logger().debug("close id={} prev_state={}", id_, static_cast<int>(prev));
    socket_->close();
}
```

3. `write_pump()`:
```cpp
// 변경: boost::asio::async_write(socket_, ...) → socket_->async_write(...)
auto [ec, bytes_written] = co_await socket_->async_write(boost::asio::buffer(req.data));
```

- [ ] **Step 3: test_helpers.hpp에 SocketBase 버전 헬퍼 추가**

```cpp
// make_socket_pair 기존 유지 (tcp::socket 쌍) +
// 새 헬퍼:
inline std::pair<std::unique_ptr<apex::core::SocketBase>, boost::asio::ip::tcp::socket>
make_session_socket_pair(boost::asio::io_context& ctx)
{
    auto [server, client] = make_socket_pair(ctx);
    return {apex::core::make_tcp_socket(std::move(server)), std::move(client)};
}
```

- [ ] **Step 4: 컴파일 확인 (Session 의존 파일만)**

이 시점에서 SessionManager, ConnectionHandler 등이 아직 `tcp::socket`을 전달하므로 컴파일 에러 예상. Task 3, 4에서 순차 해결.

- [ ] **Step 5: 커밋 (WIP — 후속 태스크와 함께 스쿼시 가능)**

```
refactor(core): Session이 unique_ptr<SocketBase>를 소유하도록 변경 (BACKLOG-133)
```

---

## Task 3: SessionManager를 SocketBase 기반으로 전환

**Files:**
- Modify: `apex_core/include/apex/core/session_manager.hpp:35`
- Modify: `apex_core/src/session_manager.cpp:20-34`
- Modify: `apex_core/tests/unit/test_session_manager.cpp`

- [ ] **Step 1: SessionManager 시그니처 변경**

```cpp
// session_manager.hpp — 변경 전:
[[nodiscard]] SessionPtr create_session(boost::asio::ip::tcp::socket socket);

// 변경 후:
[[nodiscard]] SessionPtr create_session(std::unique_ptr<SocketBase> socket);
```

`#include <boost/asio/ip/tcp.hpp>` 제거, `#include <apex/core/socket_base.hpp>` 추가.

- [ ] **Step 2: SessionManager 구현 변경**

```cpp
// session_manager.cpp — 변경 후:
SessionPtr SessionManager::create_session(std::unique_ptr<SocketBase> socket)
{
    SessionId id = make_session_id(next_id_++);
    Session* raw = session_pool_.construct(id, std::move(socket), core_id_,
                                           recv_buf_capacity_, max_queue_depth_);
    if (raw)
    {
        raw->pool_owner_ = &session_pool_;
    }
    else
    {
        raw = new Session(id, std::move(socket), core_id_,
                         recv_buf_capacity_, max_queue_depth_);
        metric_heap_fallback_.fetch_add(1, std::memory_order_relaxed);
        logger_.warn("session SlabAllocator exhausted, heap fallback");
    }
    // ... 이하 동일
}
```

**주의**: SlabAllocator 경로와 heap fallback 경로에서 `socket`은 move-only이므로 한 쪽에서만 소비된다. `session_pool_.construct` 성공 시 socket은 이미 이동 완료되어 else 분기의 `std::move(socket)`은 빈 포인터가 된다. **수정 필요**: construct 실패 시를 위해 socket을 먼저 이동하지 않도록 로직 조정.

실제 구현:
```cpp
SessionPtr SessionManager::create_session(std::unique_ptr<SocketBase> socket)
{
    SessionId id = make_session_id(next_id_++);

    // SlabAllocator::construct는 perfect forwarding하므로
    // unique_ptr move는 construct 내부에서 발생.
    // 실패 시(nullptr 반환) socket은 이동되지 않음.
    Session* raw = session_pool_.construct(id, std::move(socket), core_id_,
                                           recv_buf_capacity_, max_queue_depth_);
    if (!raw)
    {
        // SlabAllocator exhausted → heap fallback
        // 주의: 위 construct가 실패하면 socket은 아직 유효
        // — SlabAllocator::construct는 allocate 실패 시 construct를 호출하지 않으므로
        //   인자의 move는 발생하지 않음.
        raw = new Session(id, std::move(socket), core_id_,
                         recv_buf_capacity_, max_queue_depth_);
        metric_heap_fallback_.fetch_add(1, std::memory_order_relaxed);
        logger_.warn("session SlabAllocator exhausted, heap fallback");
    }
    else
    {
        raw->pool_owner_ = &session_pool_;
    }

    SessionPtr session(raw);
    session->set_state(Session::State::Active);
    sessions_[id] = session;

    if (heartbeat_timeout_ticks_ > 0)
    {
        auto timer_id = timer_wheel_.schedule(heartbeat_timeout_ticks_);
        timer_to_session_[timer_id] = id;
        session->timer_entry_id_ = timer_id;
    }

    metric_sessions_created_.fetch_add(1, std::memory_order_relaxed);
    logger_.info("session created id={} total={}", id, sessions_.size());
    return session;
}
```

- [ ] **Step 3: test_session_manager.cpp 전체 수정**

모든 `make_socket_pair` + `mgr.create_session(std::move(server))` 패턴을:
```cpp
auto [server_socket, client] = apex::test::make_session_socket_pair(io_ctx_);
auto session = mgr.create_session(std::move(server_socket));
```

로 변환. 각 테스트에서 `server` 대신 `server_socket`을 사용하고 `make_tcp_socket` wrap 적용.

- [ ] **Step 4: 빌드 확인**

이 시점에서 ConnectionHandler가 아직 `tcp::socket`을 전달하므로 ConnectionHandler 쪽 컴파일 에러 예상. Task 4에서 해결.

- [ ] **Step 5: 커밋**

```
refactor(core): SessionManager가 unique_ptr<SocketBase>를 수신하도록 변경 (BACKLOG-133)
```

---

## Task 4: ConnectionHandler + Listener 전환

**Files:**
- Modify: `apex_core/include/apex/core/connection_handler.hpp:56,105-107`
- Modify: `apex_core/include/apex/core/listener.hpp:106,119,130-132,237-238`

- [ ] **Step 1: ConnectionHandler 수정**

```cpp
// connection_handler.hpp

// accept_connection — 변경 후:
void accept_connection(std::unique_ptr<SocketBase> socket, boost::asio::io_context& io_ctx)
{
    if (config_.tcp_nodelay)
    {
        socket->set_option_no_delay(true);
    }

    auto session = session_mgr_.create_session(std::move(socket));
    session->set_core_executor(io_ctx.get_executor());
    boost::asio::co_spawn(io_ctx, read_loop(std::move(session)), boost::asio::detached);
}

// read_loop — socket 접근 변경:
// 변경 전:
auto [ec, n] = co_await session->socket().async_read_some(
    boost::asio::buffer(writable.data(), writable.size()),
    boost::asio::as_tuple(boost::asio::use_awaitable));

// 변경 후:
auto [ec, n] = co_await session->socket().async_read_some(
    boost::asio::buffer(writable.data(), writable.size()));
// SocketBase::async_read_some은 이미 as_tuple 반환
```

- [ ] **Step 2: Listener 수정 — accept 콜백에서 TcpSocket wrap**

```cpp
// listener.hpp

// reuseport 경로 — 변경 후:
[this, i](boost::asio::ip::tcp::socket socket) {
    // ... connection limit check ...
    per_core_handlers_[i]->handler.accept_connection(
        make_tcp_socket(std::move(socket)), engine_.io_context(i));
}

// single acceptor 경로 — on_accept 변경 후:
void on_accept(boost::asio::ip::tcp::socket socket)
{
    // ... connection limit check ...
    uint32_t num_cores = static_cast<uint32_t>(per_core_handlers_.size());
    uint32_t core_id = next_core_.fetch_add(1, std::memory_order_relaxed) % num_cores;

    auto& core_io = engine_.io_context(core_id);
    auto wrapped = make_tcp_socket(std::move(socket));
    boost::asio::post(core_io, [this, core_id, s = std::move(wrapped)]() mutable {
        per_core_handlers_[core_id]->handler.accept_connection(
            std::move(s), engine_.io_context(core_id));
    });
}
```

`#include <apex/core/socket_base.hpp>` 추가.

- [ ] **Step 3: 빌드 & 전체 테스트**

Run: `apex-agent queue build debug`
Expected: 빌드 성공, 기존 integration/unit 테스트 전체 PASS

- [ ] **Step 4: 커밋**

```
refactor(core): ConnectionHandler/Listener가 SocketBase를 사용하도록 전환 (BACKLOG-133)
```

---

## Task 5: TransportContext에서 ssl_ctx 제거

**Files:**
- Modify: `apex_core/include/apex/core/transport.hpp:11,21-29,37,49-50,67`
- Modify: `apex_shared/lib/protocols/tcp/include/apex/shared/protocols/tcp/tls_tcp_transport.hpp:33-37`
- Modify: `apex_shared/lib/protocols/tcp/include/apex/shared/protocols/tcp/plain_tcp_transport.hpp:21`

- [ ] **Step 1: TransportContext에서 ssl_ctx 제거**

```cpp
// transport.hpp — 변경 후:

// #include <boost/asio/ssl/context.hpp> 제거

/// Transport에 전달되는 번들 컨텍스트.
/// 향후 metrics*, buffer_pool* 등 확장 가능.
struct TransportContext
{
    // ssl_ctx 제거 — ssl::context 소유권은 Listener<P, TlsTcpTransport>로 이동
    // 향후 확장: metrics*, buffer_pool* 등
};
```

- [ ] **Step 2: Transport concept의 make_socket 시그니처 변경**

TransportContext가 비어있지만 확장성을 위해 유지한다 (향후 metrics* 등 추가 가능). concept 시그니처는 유지.

```cpp
// Transport concept — 변경 없음 (TransportContext 구조체만 변경)
```

- [ ] **Step 3: TlsTcpTransport make_socket 변경**

TransportContext에 ssl_ctx가 없으므로, TlsTcpTransport::make_socket은 더 이상 TransportContext를 통해 ssl_ctx를 받을 수 없다. 대신 Listener가 ssl::context를 직접 소유하고 make_socket에 전달하는 방식으로 변경.

두 가지 접근:
- **A**: Transport concept에 optional ssl_ctx 전달 인터페이스 추가 → concept 오염
- **B**: TlsTcpTransport에 별도 `make_socket(io_context&, ssl::context&)` 오버로드 + concept은 기존 시그니처 유지 (PlainTcp는 TransportContext 무시)

**B 채택**: concept은 건드리지 않고, TlsTcpTransport만 추가 오버로드.

```cpp
// tls_tcp_transport.hpp — 변경 후:

// 기존 concept 충족용 (TransportContext 버전) — ssl_ctx가 없으므로 assert 제거, unused
static Socket make_socket(boost::asio::io_context& ctx, const apex::core::TransportContext& /*tx_ctx*/)
{
    // concept 충족용 — 실제 호출 시 ssl_ctx 버전 사용
    // 이 경로로 호출되면 버그
    APEX_ASSERT(false, "TlsTcpTransport::make_socket requires ssl::context directly");
    // unreachable이지만 컴파일러 경고 방지
    static boost::asio::ssl::context dummy(boost::asio::ssl::context::tlsv13);
    return Socket(ctx, dummy);
}

/// Listener<P, TlsTcpTransport>가 직접 호출하는 오버로드.
static Socket make_socket(boost::asio::io_context& ctx, boost::asio::ssl::context& ssl_ctx)
{
    return Socket(ctx, ssl_ctx);
}
```

- [ ] **Step 4: 빌드 확인**

Run: `apex-agent queue build debug`
Expected: `transport.hpp`에서 `ssl/context.hpp` include가 제거되어 apex_core 이 OpenSSL에 직접 의존하지 않음을 확인.

- [ ] **Step 5: 커밋**

```
refactor(core): TransportContext에서 ssl_ctx 제거 — apex_core OpenSSL 의존 해소 (BACKLOG-133)
```

---

## Task 6: 테스트 정리 + clang-format + 최종 빌드

**Files:**
- Modify: 전체 수정 파일 clang-format
- Verify: 기존 테스트 전체 PASS

- [ ] **Step 1: clang-format 실행**

```bash
find apex_core apex_shared apex_services \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) ! -name '*_generated.h' | xargs clang-format -i
```

- [ ] **Step 2: 전체 빌드 + 테스트**

Run: `apex-agent queue build debug`
Expected: 빌드 성공, 전체 테스트 PASS, 경고 0건

- [ ] **Step 3: 최종 커밋**

```
style(core): clang-format 적용 (BACKLOG-133)
```

---

## Task 7: 문서 갱신

**Files:**
- Modify: `docs/Apex_Pipeline.md`
- Modify: `CLAUDE.md`
- Modify: `docs/apex_core/apex_core_guide.md`
- Create: `docs/apex_common/progress/<timestamp>_socket_base_abstraction.md`

- [ ] **Step 1: apex_core_guide.md 갱신**

§10 내부 아키텍처의 Session/Transport 관련 섹션에 SocketBase 설계를 반영:
- Session이 `unique_ptr<SocketBase>` 소유
- TcpSocket/TlsSocket 구현체 구조
- Transport concept은 유지, ssl_ctx는 Listener 레벨 소유

- [ ] **Step 2: Apex_Pipeline.md 갱신**

로드맵에 SocketBase 리팩터링 완료 기록. v0.6.4 설명에 반영.

- [ ] **Step 3: CLAUDE.md 갱신**

로드맵 섹션 갱신 (현재 마일스톤 설명).

- [ ] **Step 4: progress 문서 작성**

작업 요약 + 변경 파일 목록 + 설계 결정 참조.

- [ ] **Step 5: 커밋**

```
docs(core): SocketBase 리팩터링 문서 갱신 (BACKLOG-133)
```

---

## 의존 관계 & 실행 순서

```
Task 1 (SocketBase 인터페이스) ──┐
                                 ├──▶ Task 2 (Session) ──▶ Task 3 (SessionManager) ──▶ Task 4 (ConnectionHandler + Listener)
                                 │                                                            │
                                 │                                                            ▼
                                 │                                                     Task 5 (TransportContext ssl_ctx 제거)
                                 │                                                            │
                                 │                                                            ▼
                                 │                                                     Task 6 (테스트 + format)
                                 │                                                            │
                                 └────────────────────────────────────────────────────▶ Task 7 (문서)
```

Task 1은 독립. Task 2~5는 순차 의존. Task 6은 Task 5 이후. Task 7은 Task 6 이후 (또는 병렬 가능).

## 리스크 & 주의사항

1. **SlabAllocator와 unique_ptr**: `session_pool_.construct(id, std::move(socket), ...)`에서 allocate 실패 시 인자 move가 발생하지 않음을 SlabAllocator 코드에서 확인 완료 (allocate 실패 → nullptr 반환, construct 미호출).

2. **기존 Integration 테스트**: Server → Listener → ConnectionHandler 경로에서 자동으로 `make_tcp_socket` wrap이 적용되므로 integration 테스트 코드 변경 불필요.

3. **TlsSocket 구현**: 이 PR 범위 밖. SocketBase 인터페이스만 도입하고, 실제 TLS 연결 테스트는 TlsSocket 구현 PR에서 진행. TlsTcpTransport의 concept 충족용 make_socket은 유지하되 실제 호출 시 별도 오버로드 사용.

4. **async_read_some 시그니처 차이**: 기존 `session->socket().async_read_some(buffer, completion_token)` → `session->socket().async_read_some(buffer)` (SocketBase가 as_tuple을 내부 처리). ConnectionHandler의 read_loop에서 호출 방식이 달라지므로 주의.
