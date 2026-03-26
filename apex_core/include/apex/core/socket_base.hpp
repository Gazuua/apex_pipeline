// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <cstddef>
#include <memory>
#include <utility>

namespace apex::core
{

/// Virtual socket interface — 타입 소거를 통해 Session이 tcp::socket과
/// ssl::stream<tcp::socket>을 동일하게 취급할 수 있도록 한다.
///
/// 설계 근거 (BACKLOG-133 B안):
/// - Session/SessionManager 비템플릿 유지 — 템플릿 전파 방지
/// - I/O virtual dispatch ~2ns는 커널 syscall 대비 무시 가능 (0.001% 미만)
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

    /// 비동기 쓰기 — buf의 모든 데이터를 전송한다 (부분 쓰기 없음).
    /// @return {error_code, bytes_transferred}
    virtual boost::asio::awaitable<std::pair<boost::system::error_code, size_t>>
    async_write(boost::asio::const_buffer buf) = 0;

    /// TLS handshake (서버 측). Plain TCP는 no-op.
    /// ConnectionHandler::read_loop 진입 전 호출.
    virtual boost::asio::awaitable<Result<void>> async_handshake() = 0;

    /// 소켓 닫기 (shutdown + close). 에러 무시.
    virtual void close() noexcept = 0;

    /// 소켓이 열려있는지.
    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    /// 소켓의 executor — timer/write_pump co_spawn 시 필요.
    [[nodiscard]] virtual boost::asio::any_io_executor get_executor() noexcept = 0;

    /// TCP 옵션 설정 (tcp_nodelay 등). 하위 레이어 소켓에 적용.
    virtual void set_option_no_delay(bool enabled) = 0;

    /// 원격 엔드포인트 (IP+포트). 로깅, rate limiting 등에 사용.
    [[nodiscard]] virtual boost::asio::ip::tcp::endpoint
    remote_endpoint(boost::system::error_code& ec) const noexcept = 0;
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
        co_return co_await boost::asio::async_write(socket_, buf, boost::asio::as_tuple(boost::asio::use_awaitable));
    }

    boost::asio::awaitable<Result<void>> async_handshake() override
    {
        co_return ok(); // Plain TCP — handshake 불필요
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

    [[nodiscard]] boost::asio::ip::tcp::endpoint remote_endpoint(boost::system::error_code& ec) const noexcept override
    {
        return socket_.remote_endpoint(ec);
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
