// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/http_server_base.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>

namespace apex::core
{

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace
{

/// RAII guard for session tracking.
struct SessionGuard
{
    std::shared_ptr<HttpSessionTracker> tracker;
    beast::tcp_stream* stream;

    SessionGuard(std::shared_ptr<HttpSessionTracker> t, beast::tcp_stream* s)
        : tracker(std::move(t))
        , stream(s)
    {
        tracker->add(stream);
    }

    ~SessionGuard()
    {
        tracker->remove(stream);
    }

    SessionGuard(const SessionGuard&) = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;
};

} // anonymous namespace

void HttpServerBase::start(net::io_context& io, uint16_t port)
{
    tracker_ = std::make_shared<HttpSessionTracker>();

    auto endpoint = tcp::endpoint(tcp::v4(), port);
    acceptor_ = std::make_unique<tcp::acceptor>(io, endpoint);

    logger_.info("started on port {}", acceptor_->local_endpoint().port());
    do_accept();
}

void HttpServerBase::stop()
{
    if (acceptor_ && acceptor_->is_open())
    {
        beast::error_code ec;
        acceptor_->close(ec);
    }
    acceptor_.reset();

    if (tracker_)
    {
        auto count = tracker_->streams.size();
        if (count > 0)
        {
            tracker_->cancel_all();
            logger_.info("stopped ({} in-flight sessions cancelled)", count);
        }
        else
        {
            logger_.info("stopped");
        }
    }
}

void HttpServerBase::do_accept()
{
    if (!acceptor_ || !acceptor_->is_open())
        return;

    acceptor_->async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (ec)
            return; // acceptor closed or error — stop accepting
        beast::tcp_stream stream(std::move(socket));
        net::co_spawn(stream.get_executor(), run_session(std::move(stream)), net::detached);
        do_accept();
    });
}

net::awaitable<void> HttpServerBase::run_session(beast::tcp_stream stream)
{
    SessionGuard guard{tracker_, &stream};
    beast::flat_buffer buffer;

    try
    {
        stream.expires_after(std::chrono::seconds{5});

        http::request<http::empty_body> req;
        co_await http::async_read(stream, buffer, req, net::use_awaitable);

        // Delegate to derived class
        auto target = std::string_view(req.target());
        auto result = handle_request(req.method(), target);

        // Build Beast response from HttpResponse
        http::response<http::string_body> res;
        res.version(req.version());
        res.keep_alive(false);
        res.result(static_cast<http::status>(result.status_code));
        res.set(http::field::content_type, result.content_type);
        res.body() = std::move(result.body);
        res.prepare_payload();

        stream.expires_after(std::chrono::seconds{5});
        co_await http::async_write(stream, res, net::use_awaitable);

        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    }
    catch (const std::exception& e)
    {
        logger_.debug("session ended: {}", e.what());
    }
}

} // namespace apex::core
