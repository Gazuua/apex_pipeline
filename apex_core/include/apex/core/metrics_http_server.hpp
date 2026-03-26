// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/http_server_base.hpp>

#include <atomic>

namespace apex::core
{

class MetricsRegistry;

/// Lightweight HTTP server for Prometheus /metrics scraping.
/// Runs on Server's control_io_ (single-threaded, non-blocking).
/// Endpoints: GET /metrics, GET /health, GET /ready.
class MetricsHttpServer : public HttpServerBase
{
  public:
    MetricsHttpServer();

    /// Bind dependencies and start accepting.
    /// @param io       control_io_ from Server
    /// @param port     HTTP listen port (e.g., 8081)
    /// @param registry MetricsRegistry reference for /metrics serialization
    /// @param running  Server's running_ flag for /ready endpoint
    void start(boost::asio::io_context& io, uint16_t port, MetricsRegistry& registry, const std::atomic<bool>& running);

  protected:
    [[nodiscard]] HttpResponse handle_request(boost::beast::http::verb method, std::string_view target) override;

  private:
    MetricsRegistry* registry_{nullptr};
    const std::atomic<bool>* running_{nullptr};
};

} // namespace apex::core
