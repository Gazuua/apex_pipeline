// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/metrics_http_server.hpp>
#include <apex/core/metrics_registry.hpp>

namespace apex::core
{

namespace http = boost::beast::http;

MetricsHttpServer::MetricsHttpServer()
{
    logger_ = ScopedLogger{"MetricsHttpServer", ScopedLogger::NO_CORE};
}

void MetricsHttpServer::start(boost::asio::io_context& io, uint16_t port, MetricsRegistry& registry,
                              const std::atomic<bool>& running)
{
    registry_ = &registry;
    running_ = &running;
    HttpServerBase::start(io, port);
}

HttpResponse MetricsHttpServer::handle_request(http::verb method, std::string_view target)
{
    if (method == http::verb::get && target == "/metrics")
    {
        return {200, "text/plain; version=0.0.4; charset=utf-8", registry_->serialize()};
    }

    if (method == http::verb::get && target == "/health")
    {
        return {200, "text/plain", "OK\n"};
    }

    if (method == http::verb::get && target == "/ready")
    {
        if (running_->load(std::memory_order_acquire))
        {
            return {200, "text/plain", "READY\n"};
        }
        return {503, "text/plain", "NOT READY\n"};
    }

    return {404, "text/plain", "Not Found\n"};
}

} // namespace apex::core
