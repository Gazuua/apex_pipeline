// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/transport.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace apex::shared::protocols::tcp
{

/// Plain TCP transport — TLS 없는 기본 TCP 연결.
struct PlainTcpTransport
{
    struct Config
    {};
    using Socket = boost::asio::ip::tcp::socket;

    static Socket make_socket(boost::asio::io_context& ctx)
    {
        return Socket(ctx);
    }

    [[nodiscard]] static boost::asio::awaitable<apex::core::Result<void>>
    async_accept(boost::asio::ip::tcp::acceptor& acceptor, Socket& sock)
    {
        auto [ec] = co_await acceptor.async_accept(sock, boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec)
            co_return apex::core::error(apex::core::ErrorCode::AcceptFailed);
        co_return apex::core::ok();
    }

    [[nodiscard]] static boost::asio::awaitable<apex::core::Result<void>> async_handshake(Socket&, const Config&)
    {
        // Plain TCP — handshake is no-op
        co_return apex::core::ok();
    }

    static boost::asio::awaitable<void> async_shutdown(Socket& sock)
    {
        boost::system::error_code ec;
        sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        co_return;
    }
};

// Concept 검증
static_assert(apex::core::Transport<PlainTcpTransport>);

} // namespace apex::shared::protocols::tcp
