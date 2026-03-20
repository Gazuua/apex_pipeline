// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/transport.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <spdlog/spdlog.h>

#include <string>

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

    static Socket make_socket(boost::asio::io_context& ctx)
    {
        // Default SSL context -- for concept satisfaction / test use.
        // Real path uses make_socket_with_context with per-core ssl::context.
        static boost::asio::ssl::context default_ctx(boost::asio::ssl::context::tlsv13);
        return Socket(ctx, default_ctx);
    }

    /// Create socket with per-core SSL context (production path).
    static Socket make_socket_with_context(boost::asio::io_context& ctx, boost::asio::ssl::context& ssl_ctx)
    {
        return Socket(ctx, ssl_ctx);
    }

    /// Initialize SSL context (per-core).
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
            spdlog::warn("TLS accept failed: {}", ec.message());
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
            spdlog::warn("TLS handshake failed: {}", ec.message());
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
};

// Concept verification
static_assert(apex::core::Transport<TlsTcpTransport>);

} // namespace apex::shared::protocols::tcp
