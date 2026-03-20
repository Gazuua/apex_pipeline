// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/bump_allocator.hpp>
#include <apex/core/result.hpp>
#include <apex/shared/adapters/adapter_error.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>
#include <apex/shared/adapters/pg/pg_result.hpp>

#include <libpq-fe.h>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::shared::adapters::pg
{

/// libpq async connection integrated with Boost.Asio event loop.
///
/// Core technique: PQsocket() fd is assigned to boost::asio::ip::tcp::socket,
/// registering it with IOCP (Windows) / epoll (Linux). All I/O is driven
/// by the Asio event loop.
///
/// Usage:
///   PgConnection conn(io_ctx);
///   co_await conn.connect_async(conninfo);
///   auto result = co_await conn.query_async("SELECT ...");
///
/// Thread safety: NOT thread-safe. Per-core PgPool ensures single-thread access.
class PgConnection
{
  public:
    explicit PgConnection(boost::asio::io_context& io_ctx, apex::core::BumpAllocator* request_alloc = nullptr);
    ~PgConnection();

    // Non-copyable, movable
    PgConnection(const PgConnection&) = delete;
    PgConnection& operator=(const PgConnection&) = delete;
    PgConnection(PgConnection&& other) noexcept;
    PgConnection& operator=(PgConnection&& other) noexcept;

    /// Async connection establishment.
    /// PQconnectStart -> PQsocket -> Asio assign -> PQconnectPoll loop.
    /// @param conninfo libpq connection string (e.g., "host=localhost port=6432 dbname=apex")
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> connect_async(std::string_view conninfo);

    /// Async query execution (no parameters).
    /// PQsendQuery -> Asio readable -> PQconsumeInput -> PQgetResult.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>> query_async(std::string_view sql);

    /// Async parameterized query execution.
    /// PQsendQueryParams -> Asio readable -> PQconsumeInput -> PQgetResult.
    /// @param sql SQL with $1, $2, ... placeholders
    /// @param params Parameter values (text format)
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>>
    query_params_async(std::string_view sql, std::span<const std::string> params);

    /// Async command execution (INSERT/UPDATE/DELETE with no result rows).
    /// Internally calls query_async() and returns affected_rows.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<int>> execute_async(std::string_view sql);

    /// Async parameterized command execution.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<int>>
    execute_params_async(std::string_view sql, std::span<const std::string> params);

    /// Async prepared statement creation.
    /// PQsendPrepare -> Asio readable -> PQconsumeInput -> PQgetResult.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> prepare_async(std::string_view name,
                                                                                 std::string_view sql);

    /// Async prepared statement execution.
    /// PQsendQueryPrepared -> Asio readable -> PQconsumeInput -> PQgetResult.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>>
    query_prepared_async(std::string_view name, std::span<const std::string> params);

    /// Connection validity check (PQstatus).
    [[nodiscard]] bool is_valid() const noexcept;

    /// Close connection. Releases fd from Asio + PQfinish.
    void close() noexcept;

    /// Connection state
    [[nodiscard]] bool is_connected() const noexcept;

    /// Poisoned state — connection should not be returned to pool.
    /// Set when an unfinished transaction is detected on destruction.
    void mark_poisoned() noexcept
    {
        poisoned_ = true;
    }
    [[nodiscard]] bool is_poisoned() const noexcept
    {
        return poisoned_;
    }

  private:
    /// PQconnectPoll loop (internal to connect_async)
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> poll_connect();

    /// Query result collection loop (internal to query_async/execute_async)
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>> collect_result();

    /// Release Asio socket without closing the fd (libpq owns the socket)
    void release_socket() noexcept;

    boost::asio::io_context& io_ctx_;
    apex::core::BumpAllocator* request_alloc_{nullptr};
    PGconn* conn_ = nullptr;
    /// libpq fd registered with Asio for IOCP/epoll integration.
    /// Windows: tcp::socket::assign(tcp::v4(), PQsocket(conn)) -> IOCP
    /// Linux:   tcp::socket::assign(tcp::v4(), PQsocket(conn)) -> epoll
    std::unique_ptr<boost::asio::ip::tcp::socket> socket_;
    bool connected_ = false;
    bool poisoned_ = false;
};

} // namespace apex::shared::adapters::pg
