// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/http_server_base.hpp>

namespace apex::core
{

/// Admin HTTP server for runtime management endpoints.
/// Runs on Server's control_io_ (separate port from MetricsHttpServer).
/// Localhost-only binding — no external access.
///
/// Endpoints:
///   GET  /admin/log-level  — query current log level
///   POST /admin/log-level  — change log level at runtime
class AdminHttpServer : public HttpServerBase
{
  public:
    AdminHttpServer();

  protected:
    [[nodiscard]] HttpResponse handle_request(boost::beast::http::verb method, std::string_view target) override;

  private:
    [[nodiscard]] HttpResponse handle_log_level_get(std::string_view query) const;
    [[nodiscard]] HttpResponse handle_log_level_post(std::string_view query);

    /// Parse query string parameter value. Returns empty if not found.
    [[nodiscard]] static std::string parse_query_param(std::string_view query, std::string_view key);
};

} // namespace apex::core
