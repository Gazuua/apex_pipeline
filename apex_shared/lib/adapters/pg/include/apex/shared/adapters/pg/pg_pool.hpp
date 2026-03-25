// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

// pg_pool.hpp — self-contained (CRTP ConnectionPool inheritance removed)
#pragma once

#include <apex/core/result.hpp>
#include <apex/core/scoped_logger.hpp>
#include <apex/shared/adapters/pg/pg_connection.hpp>
#include <apex/shared/adapters/pool_concept.hpp>
#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace apex::shared::adapters::pg
{

struct PgAdapterConfig; // forward

class PgPool
{
  public:
    using Connection = std::unique_ptr<PgConnection>;

    PgPool(boost::asio::io_context& io_ctx, const PgAdapterConfig& config);
    ~PgPool();

    PgPool(const PgPool&) = delete;
    PgPool& operator=(const PgPool&) = delete;

    // --- SyncPoolLike concept ---
    [[nodiscard]] apex::core::Result<Connection> acquire();
    void release(Connection conn);
    void discard(Connection conn);
    void close_all();
    [[nodiscard]] PoolStats stats() const noexcept;

    // --- Pool maintenance ---
    void shrink_idle();
    void health_check_tick();

    [[nodiscard]] std::size_t active_count() const noexcept;
    [[nodiscard]] std::size_t idle_count() const noexcept;
    [[nodiscard]] std::size_t total_count() const noexcept;
    [[nodiscard]] const PoolConfig& config() const noexcept;

    // --- PG-specific async API (outside SyncPoolLike concept scope) ---
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<Connection>> create_connected();

    [[nodiscard]] boost::asio::awaitable<apex::core::Result<Connection>> acquire_connected();

    /// Retry-aware acquire: wraps acquire() with exponential backoff.
    /// Returns PoolExhausted on retry limit exceeded.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<Connection>> acquire_with_retry();

    [[nodiscard]] const std::string& connection_string() const noexcept;

    /// Increment query counter (called by PgAdapter on each query/execute).
    void record_query() noexcept
    {
        metric_queries_total_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Metric atomic access (for MetricsRegistry::counter_from)
    [[nodiscard]] const std::atomic<uint64_t>& metric_queries_total() const noexcept
    {
        return metric_queries_total_;
    }

  private:
    Connection do_create_connection();
    void do_destroy_connection(Connection& conn);
    bool do_validate(Connection& conn);

    struct IdleEntry
    {
        Connection conn;
        std::chrono::steady_clock::time_point returned_at;
    };

    apex::core::ScopedLogger logger_{"PgPool", apex::core::ScopedLogger::NO_CORE, "app"};
    boost::asio::io_context& io_ctx_;
    const PgAdapterConfig& config_;
    PoolConfig pool_config_;
    std::deque<IdleEntry> idle_;
    std::size_t total_count_{0};
    std::size_t active_count_{0};
    PoolStats stats_;
    std::atomic<uint64_t> metric_queries_total_{0};
};

// Concept verification
static_assert(SyncPoolLike<PgPool>);

} // namespace apex::shared::adapters::pg
