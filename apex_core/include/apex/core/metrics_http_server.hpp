// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/scoped_logger.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cstdint>
#include <memory>

namespace apex::core
{

class MetricsRegistry;

/// Lightweight HTTP server for Prometheus /metrics scraping.
/// Runs on Server's control_io_ (single-threaded, non-blocking).
/// Endpoints: GET /metrics, GET /health, GET /ready.
class MetricsHttpServer
{
  public:
    MetricsHttpServer() = default;

    /// Start accepting HTTP connections on the given port.
    /// @param io       control_io_ from Server (signal/shutdown io_context)
    /// @param port     HTTP listen port (e.g., 8081)
    /// @param registry MetricsRegistry reference for /metrics serialization
    /// @param running  Server's running_ flag for /ready endpoint
    void start(boost::asio::io_context& io, uint16_t port, MetricsRegistry& registry, const std::atomic<bool>& running);

    /// Stop accepting new connections and close the acceptor.
    void stop();

  private:
    void do_accept();

    boost::asio::ip::tcp::acceptor* acceptor_{nullptr};
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_storage_;
    MetricsRegistry* registry_{nullptr};
    const std::atomic<bool>* running_{nullptr};
    ScopedLogger logger_{"MetricsHttpServer", ScopedLogger::NO_CORE};
};

} // namespace apex::core
