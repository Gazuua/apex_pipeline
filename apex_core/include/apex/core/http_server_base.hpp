// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/scoped_logger.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace apex::core
{

/// Minimal HTTP response struct returned by derived handle_request().
struct HttpResponse
{
    unsigned status_code = 200;
    std::string content_type = "text/plain";
    std::string body;
};

/// In-flight session tracking for graceful drain.
/// All access is single-threaded (control_io_), no synchronization needed.
struct HttpSessionTracker
{
    std::vector<boost::beast::tcp_stream*> streams;

    void add(boost::beast::tcp_stream* s)
    {
        streams.push_back(s);
    }

    void remove(boost::beast::tcp_stream* s)
    {
        std::erase(streams, s);
    }

    void cancel_all()
    {
        for (auto* s : streams)
        {
            boost::beast::error_code ec;
            s->socket().cancel(ec);
        }
    }
};

/// Base class for lightweight HTTP servers running on Server's control_io_.
/// Provides accept loop, session tracking, HTTP request/response handling.
/// Derived classes override handle_request() for routing.
///
/// Uses Boost.Beast for HTTP parsing with coroutine-based session handling.
/// Single-request-per-connection (Connection: close).
class HttpServerBase
{
  public:
    HttpServerBase() = default;
    virtual ~HttpServerBase() = default;

    HttpServerBase(const HttpServerBase&) = delete;
    HttpServerBase& operator=(const HttpServerBase&) = delete;

    /// Start accepting HTTP connections on the given port.
    /// @param io  control_io_ from Server
    /// @param port HTTP listen port (0 for OS-assigned)
    void start(boost::asio::io_context& io, uint16_t port);

    /// Stop accepting new connections and cancel in-flight sessions.
    void stop();

    /// Bound local port (useful when port 0 is used).
    [[nodiscard]] uint16_t local_port() const
    {
        return acceptor_ ? acceptor_->local_endpoint().port() : 0;
    }

  protected:
    /// Override to implement request routing.
    /// Called on control_io_ thread from the session coroutine.
    /// @param method  HTTP method (GET, POST, etc.)
    /// @param target  Full request target including query string (e.g., "/admin/log-level?logger=apex")
    [[nodiscard]] virtual HttpResponse handle_request(boost::beast::http::verb method, std::string_view target) = 0;

    ScopedLogger logger_;

  private:
    void do_accept();
    boost::asio::awaitable<void> run_session(boost::beast::tcp_stream stream);

    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::shared_ptr<HttpSessionTracker> tracker_;
};

} // namespace apex::core
