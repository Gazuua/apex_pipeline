// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/metrics_http_server.hpp>
#include <apex/core/metrics_registry.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

namespace apex::core
{

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

/// In-flight session tracking for graceful drain.
/// All access is single-threaded (control_io_), no synchronization needed.
struct MetricsSessionTracker
{
    std::vector<beast::tcp_stream*> streams;

    void add(beast::tcp_stream* s)
    {
        streams.push_back(s);
    }

    void remove(beast::tcp_stream* s)
    {
        std::erase(streams, s);
    }

    void cancel_all()
    {
        for (auto* s : streams)
        {
            beast::error_code ec;
            s->socket().cancel(ec);
        }
    }
};

namespace
{

/// RAII guard for session tracking.
struct SessionGuard
{
    std::shared_ptr<MetricsSessionTracker> tracker;
    beast::tcp_stream* stream;

    SessionGuard(std::shared_ptr<MetricsSessionTracker> t, beast::tcp_stream* s)
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

/// Handle a single HTTP request on the metrics endpoint.
/// Uses beast::tcp_stream with read/write timeout for slowloris protection.
net::awaitable<void> handle_session(beast::tcp_stream stream, MetricsRegistry& registry,
                                    const std::atomic<bool>& running, std::shared_ptr<MetricsSessionTracker> tracker)
{
    SessionGuard guard{std::move(tracker), &stream};
    beast::flat_buffer buffer;

    try
    {
        stream.expires_after(std::chrono::seconds{5});

        http::request<http::empty_body> req;
        co_await http::async_read(stream, buffer, req, net::use_awaitable);

        http::response<http::string_body> res;
        res.version(req.version());
        res.keep_alive(false);

        if (req.method() == http::verb::get && req.target() == "/metrics")
        {
            res.result(http::status::ok);
            res.set(http::field::content_type, "text/plain; version=0.0.4; charset=utf-8");
            res.body() = registry.serialize();
        }
        else if (req.method() == http::verb::get && req.target() == "/health")
        {
            res.result(http::status::ok);
            res.set(http::field::content_type, "text/plain");
            res.body() = "OK\n";
        }
        else if (req.method() == http::verb::get && req.target() == "/ready")
        {
            if (running.load(std::memory_order_acquire))
            {
                res.result(http::status::ok);
                res.body() = "READY\n";
            }
            else
            {
                res.result(http::status::service_unavailable);
                res.body() = "NOT READY\n";
            }
            res.set(http::field::content_type, "text/plain");
        }
        else
        {
            res.result(http::status::not_found);
            res.set(http::field::content_type, "text/plain");
            res.body() = "Not Found\n";
        }

        res.prepare_payload();

        stream.expires_after(std::chrono::seconds{5});
        co_await http::async_write(stream, res, net::use_awaitable);

        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    }
    catch (const std::exception&)
    {
        // Client disconnected, timeout, or malformed request — silently drop.
    }
}

} // anonymous namespace

void MetricsHttpServer::start(net::io_context& io, uint16_t port, MetricsRegistry& registry,
                              const std::atomic<bool>& running)
{
    registry_ = &registry;
    running_ = &running;
    tracker_ = std::make_shared<MetricsSessionTracker>();

    auto endpoint = tcp::endpoint(tcp::v4(), port);
    acceptor_ = std::make_unique<tcp::acceptor>(io, endpoint);

    logger_.info("started on port {}", port);
    do_accept();
}

void MetricsHttpServer::stop()
{
    if (acceptor_ && acceptor_->is_open())
    {
        beast::error_code ec;
        acceptor_->close(ec);
    }
    acceptor_.reset();

    // Cancel in-flight sessions so they don't race with adapter shutdown
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

void MetricsHttpServer::do_accept()
{
    if (!acceptor_ || !acceptor_->is_open())
        return;

    acceptor_->async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (ec)
            return; // acceptor closed or error — stop accepting
        beast::tcp_stream stream(std::move(socket));
        net::co_spawn(stream.get_executor(), handle_session(std::move(stream), *registry_, *running_, tracker_),
                      net::detached);
        do_accept();
    });
}

} // namespace apex::core
