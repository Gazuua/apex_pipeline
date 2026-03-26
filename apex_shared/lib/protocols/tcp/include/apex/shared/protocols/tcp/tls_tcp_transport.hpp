// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/assert.hpp>
#include <apex/core/scoped_logger.hpp>
#include <apex/core/socket_base.hpp>
#include <apex/core/transport.hpp>
#include <apex/shared/protocols/tcp/tls_socket.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <memory>
#include <string>
#include <utility>

namespace apex::shared::protocols::tcp
{

/// TLS TCP transport -- OpenSSL-based TLS connections.
/// Per-core SSL_CTX support (Seastar style, eliminates lock contention).
struct TlsTcpTransport
{
    struct Config
    {
        std::string cert_file;
        std::string key_file;
        std::string ca_file;
        bool require_client_cert = false;
    };

    using Socket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

    /// Listener가 소유하는 TLS 상태 — ssl::context를 보유.
    struct ListenerState
    {
        boost::asio::ssl::context ssl_ctx;
        explicit ListenerState(boost::asio::ssl::context ctx)
            : ssl_ctx(std::move(ctx))
        {}
    };

    static Socket make_socket(boost::asio::io_context& ctx)
    {
        // concept 충족용 — 실제 TLS 소켓 생성은 wrap_socket 경유.
        // 이 경로는 직접 호출되지 않음.
        APEX_ASSERT(false, "TlsTcpTransport::make_socket: use wrap_socket instead");
        // unreachable이지만 컴파일러 경고 방지
        static boost::asio::ssl::context dummy(boost::asio::ssl::context::tlsv13);
        return Socket(ctx, dummy);
    }

    /// Initialize SSL context.
    /// @return Initialized ssl::context. Caller manages lifetime.
    [[nodiscard]] static boost::asio::ssl::context create_ssl_context(const Config& cfg)
    {
        boost::asio::ssl::context ctx(boost::asio::ssl::context::tlsv13);

        ctx.set_options(boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 |
                        boost::asio::ssl::context::no_sslv3 | boost::asio::ssl::context::no_tlsv1 |
                        boost::asio::ssl::context::no_tlsv1_1 | boost::asio::ssl::context::single_dh_use);

        ctx.use_certificate_chain_file(cfg.cert_file);
        ctx.use_private_key_file(cfg.key_file, boost::asio::ssl::context::pem);

        if (!cfg.ca_file.empty())
        {
            ctx.load_verify_file(cfg.ca_file);
        }

        if (cfg.require_client_cert)
        {
            ctx.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert);
        }

        return ctx;
    }

    [[nodiscard]] static boost::asio::awaitable<apex::core::Result<void>>
    async_accept(boost::asio::ip::tcp::acceptor& acceptor, Socket& sock)
    {
        auto [ec] =
            co_await acceptor.async_accept(sock.lowest_layer(), boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec)
        {
            static const apex::core::ScopedLogger s_logger{"TlsTcpTransport", apex::core::ScopedLogger::NO_CORE, "app"};
            s_logger.warn("TLS accept failed: {}", ec.message());
            co_return apex::core::error(apex::core::ErrorCode::AcceptFailed);
        }
        co_return apex::core::ok();
    }

    [[nodiscard]] static boost::asio::awaitable<apex::core::Result<void>> async_handshake(Socket& sock, const Config&)
    {
        auto [ec] = co_await sock.async_handshake(boost::asio::ssl::stream_base::server,
                                                  boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec)
        {
            static const apex::core::ScopedLogger s_logger{"TlsTcpTransport", apex::core::ScopedLogger::NO_CORE, "app"};
            s_logger.warn("TLS handshake failed: {}", ec.message());
            co_return apex::core::error(apex::core::ErrorCode::HandshakeFailed);
        }
        co_return apex::core::ok();
    }

    static boost::asio::awaitable<void> async_shutdown(Socket& sock)
    {
        // SSL shutdown is bidirectional; may timeout.
        // Ignore errors -- client may have already disconnected.
        auto [ec] = co_await sock.async_shutdown(boost::asio::as_tuple(boost::asio::use_awaitable));
        co_return;
    }

    static ListenerState make_listener_state(const Config& cfg)
    {
        return ListenerState{create_ssl_context(cfg)};
    }

    static std::unique_ptr<apex::core::SocketBase> wrap_socket(boost::asio::ip::tcp::socket socket,
                                                               ListenerState& state)
    {
        return make_tls_socket(std::move(socket), state.ssl_ctx);
    }
};

// Concept verification
static_assert(apex::core::Transport<TlsTcpTransport>);

} // namespace apex::shared::protocols::tcp
