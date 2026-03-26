// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>
#include <apex/core/socket_base.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <memory>
#include <utility>

namespace apex::shared::protocols::tcp
{

/// TLS 소켓 래퍼 — ssl::stream<tcp::socket>을 SocketBase로 타입 소거.
/// apex_shared에 배치하여 apex_core의 OpenSSL 의존을 방지한다.
class TlsSocket final : public apex::core::SocketBase
{
  public:
    explicit TlsSocket(boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream)
        : stream_(std::move(stream))
    {}

    boost::asio::awaitable<std::pair<boost::system::error_code, size_t>>
    async_read_some(boost::asio::mutable_buffer buf) override
    {
        co_return co_await stream_.async_read_some(buf, boost::asio::as_tuple(boost::asio::use_awaitable));
    }

    boost::asio::awaitable<std::pair<boost::system::error_code, size_t>>
    async_write(boost::asio::const_buffer buf) override
    {
        co_return co_await boost::asio::async_write(stream_, buf, boost::asio::as_tuple(boost::asio::use_awaitable));
    }

    boost::asio::awaitable<apex::core::Result<void>> async_handshake() override
    {
        auto [ec] = co_await stream_.async_handshake(boost::asio::ssl::stream_base::server,
                                                     boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec)
            co_return apex::core::error(apex::core::ErrorCode::HandshakeFailed);
        co_return apex::core::ok();
    }

    void close() noexcept override
    {
        boost::system::error_code ec;
        // SSL 동기 shutdown (best effort) — close()가 noexcept이므로 async 불가
        stream_.shutdown(ec);
        if (stream_.lowest_layer().is_open())
        {
            stream_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            stream_.lowest_layer().close(ec);
        }
    }

    [[nodiscard]] bool is_open() const noexcept override
    {
        return stream_.lowest_layer().is_open();
    }

    [[nodiscard]] boost::asio::any_io_executor get_executor() noexcept override
    {
        return stream_.lowest_layer().get_executor();
    }

    void set_option_no_delay(bool enabled) override
    {
        boost::system::error_code ec;
        stream_.lowest_layer().set_option(boost::asio::ip::tcp::no_delay(enabled), ec);
    }

    [[nodiscard]] boost::asio::ip::tcp::endpoint remote_endpoint(boost::system::error_code& ec) const noexcept override
    {
        return stream_.lowest_layer().remote_endpoint(ec);
    }

  private:
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream_;
};

/// 헬퍼: raw tcp::socket + ssl::context → TlsSocket (unique_ptr<SocketBase>)
inline std::unique_ptr<apex::core::SocketBase> make_tls_socket(boost::asio::ip::tcp::socket socket,
                                                               boost::asio::ssl::context& ssl_ctx)
{
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(std::move(socket), ssl_ctx);
    return std::make_unique<TlsSocket>(std::move(stream));
}

} // namespace apex::shared::protocols::tcp
