// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/metrics_http_server.hpp>
#include <apex/core/metrics_registry.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace apex::core
{

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace
{

/// Handle a single HTTP request on the metrics endpoint.
net::awaitable<void> handle_session(tcp::socket socket, MetricsRegistry& registry, const std::atomic<bool>& running)
{
    beast::flat_buffer buffer;

    try
    {
        http::request<http::empty_body> req;
        co_await http::async_read(socket, buffer, req, net::use_awaitable);

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
        co_await http::async_write(socket, res, net::use_awaitable);

        beast::error_code ec;
        socket.shutdown(tcp::socket::shutdown_send, ec);
    }
    catch (const std::exception&)
    {
        // Client disconnected or malformed request — silently drop.
    }
}

} // anonymous namespace

void MetricsHttpServer::start(net::io_context& io, uint16_t port, MetricsRegistry& registry,
                              const std::atomic<bool>& running)
{
    registry_ = &registry;
    running_ = &running;

    auto endpoint = tcp::endpoint(tcp::v4(), port);
    acceptor_storage_ = std::make_unique<tcp::acceptor>(io, endpoint);
    acceptor_ = acceptor_storage_.get();

    logger_.info("started on port {}", port);
    do_accept();
}

void MetricsHttpServer::stop()
{
    if (acceptor_ && acceptor_->is_open())
    {
        beast::error_code ec;
        acceptor_->close(ec);
        logger_.info("stopped");
    }
    acceptor_ = nullptr;
}

void MetricsHttpServer::do_accept()
{
    if (!acceptor_ || !acceptor_->is_open())
        return;

    acceptor_->async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (ec)
            return; // acceptor closed or error — stop accepting
        net::co_spawn(socket.get_executor(), handle_session(std::move(socket), *registry_, *running_), net::detached);
        do_accept();
    });
}

} // namespace apex::core
