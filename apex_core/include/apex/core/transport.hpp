// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <concepts>
#include <cstddef>
#include <span>

namespace apex::core
{

/// Transport에 전달되는 번들 컨텍스트.
/// make_socket() 시그니처를 확장 가능하게 유지하면서
/// concept 자체는 변경하지 않아도 되도록 한다.
/// TLS Transport는 ssl_ctx를 사용하고, Plain TCP Transport는 무시한다.
struct TransportContext
{
    boost::asio::ssl::context* ssl_ctx = nullptr;
    // 향후 확장: metrics*, buffer_pool* 등
};

/// Transport concept — core에서 정의, shared에서 구현.
/// 의존성 역전: core는 concept만, 구체 Transport는 shared가 제공.
///
/// 요구사항:
///   - T::Config       — Transport별 설정 타입
///   - T::Socket       — 소켓 타입 (tcp::socket 또는 ssl::stream<tcp::socket>)
///   - T::make_socket(io_context&, const TransportContext&) -> Socket
///   - T::async_accept(acceptor, socket) -> awaitable<Result<void>>
///   - T::async_handshake(socket, config) -> awaitable<Result<void>>
///   - T::async_shutdown(socket) -> awaitable<void>
///
/// Note: async_read/write는 Socket이 AsyncReadStream/AsyncWriteStream을
/// 만족하면 Boost.Asio의 async_read/async_write가 직접 사용 가능하므로
/// concept 요구사항에서 제외한다.
template <typename T>
concept Transport = requires {
    typename T::Config;
    typename T::Socket;
} && requires(boost::asio::io_context& io_ctx, const TransportContext& tx_ctx) {
    { T::make_socket(io_ctx, tx_ctx) } -> std::same_as<typename T::Socket>;
} && requires(boost::asio::ip::tcp::acceptor& acceptor, typename T::Socket& sock) {
    { T::async_accept(acceptor, sock) } -> std::same_as<boost::asio::awaitable<Result<void>>>;
} && requires(typename T::Socket& sock, const typename T::Config& cfg) {
    { T::async_handshake(sock, cfg) } -> std::same_as<boost::asio::awaitable<Result<void>>>;
} && requires(typename T::Socket& sock) {
    { T::async_shutdown(sock) } -> std::same_as<boost::asio::awaitable<void>>;
};

/// 기본 Transport — Plain TCP (TLS 없음).
/// core 내부 정의로 순환 의존 방지. shared::PlainTcpTransport와 동일한 구현.
struct DefaultTransport
{
    struct Config
    {};
    using Socket = boost::asio::ip::tcp::socket;

    static Socket make_socket(boost::asio::io_context& ctx, const TransportContext& /*tx_ctx*/)
    {
        return Socket(ctx);
    }

    [[nodiscard]] static boost::asio::awaitable<Result<void>> async_accept(boost::asio::ip::tcp::acceptor& acc,
                                                                           Socket& sock)
    {
        auto [ec] = co_await acc.async_accept(sock, boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec)
            co_return error(ErrorCode::AcceptFailed);
        co_return ok();
    }

    [[nodiscard]] static boost::asio::awaitable<Result<void>> async_handshake(Socket&, const Config&)
    {
        co_return ok();
    }

    static boost::asio::awaitable<void> async_shutdown(Socket& sock)
    {
        boost::system::error_code ec;
        sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        co_return;
    }
};

static_assert(Transport<DefaultTransport>, "DefaultTransport must satisfy Transport concept");

// --- 컴파일타임 검증용 Mock ---
namespace detail
{

struct MockTransport
{
    struct Config
    {};
    using Socket = boost::asio::ip::tcp::socket;

    static Socket make_socket(boost::asio::io_context& ctx, const TransportContext& /*tx_ctx*/)
    {
        return Socket(ctx);
    }

    static boost::asio::awaitable<Result<void>> async_accept(boost::asio::ip::tcp::acceptor& /*acc*/, Socket& /*sock*/)
    {
        co_return ok();
    }

    static boost::asio::awaitable<Result<void>> async_handshake(Socket&, const Config&)
    {
        co_return ok();
    }

    static boost::asio::awaitable<void> async_shutdown(Socket&)
    {
        co_return;
    }
};

static_assert(Transport<MockTransport>, "MockTransport must satisfy Transport concept");

} // namespace detail

} // namespace apex::core
