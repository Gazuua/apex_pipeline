// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/scoped_logger.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>
#include <apex/shared/adapters/pg/pg_pool.hpp>
#include <apex/shared/adapters/pg/pg_result.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::shared::adapters::pg
{

/// PostgreSQL adapter -- registered as an infrastructure component on Server.
///
/// Usage:
///   Server({.port = 9000, .num_cores = 4})
///       .add_adapter<PgAdapter>(pg_config)
///       .add_service<MyService>()
///       .run();
///
///   // In service handler:
///   auto& pg = server.adapter<PgAdapter>();
///   auto result = co_await pg.query("SELECT * FROM users WHERE id = $1", {"42"});
///
/// Ownership: single global instance. Internally manages per-core PgPool.
/// CoreEngine::current_core_id() routes to the current core's pool.
class PgAdapter : public AdapterBase<PgAdapter>
{
  public:
    PgAdapter();
    explicit PgAdapter(PgAdapterConfig config);
    ~PgAdapter();

    // --- AdapterBase CRTP requirements ---
    void do_init(apex::core::CoreEngine& engine);
    void do_drain();
    void do_close();
    [[nodiscard]] std::string_view do_name() const noexcept
    {
        return "pg";
    }

    /// Register Prometheus metrics for PG query counters and pool gauges.
    void do_register_metrics(apex::core::MetricsRegistry& registry);

    // --- Query API (coroutine) ---

    /// Execute SQL query (no parameters)
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>> query(std::string_view sql);

    /// Execute parameterized query ($1, $2, ... placeholders)
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>> query(std::string_view sql,
                                                                             std::span<const std::string> params);

    /// Execute command (INSERT/UPDATE/DELETE) -- returns affected_rows
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<int>> execute(std::string_view sql);

    /// Execute parameterized command
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<int>> execute(std::string_view sql,
                                                                          std::span<const std::string> params);

    // --- Monitoring API ---

    /// Active connections across all cores
    [[nodiscard]] size_t active_connections() const noexcept;
    /// Idle connections across all cores
    [[nodiscard]] size_t idle_connections() const noexcept;

    /// Config access
    [[nodiscard]] const PgAdapterConfig& config() const noexcept
    {
        return config_;
    }

    /// Per-core pool access (test/advanced)
    [[nodiscard]] PgPool& pool(uint32_t core_id);

  private:
    /// Return the PgPool for the current core
    [[nodiscard]] PgPool& current_pool();

    apex::core::ScopedLogger logger_{"PgAdapter", apex::core::ScopedLogger::NO_CORE, "app"};
    PgAdapterConfig config_;
    /// pools_[core_id] = per-core PgPool
    std::vector<std::unique_ptr<PgPool>> pools_;
};

} // namespace apex::shared::adapters::pg
