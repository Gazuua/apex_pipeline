// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/shared/adapters/pg/pg_connection.hpp>
#include <apex/shared/adapters/pg/pg_pool.hpp>
#include <boost/asio/awaitable.hpp>
#include <span>
#include <string>
#include <string_view>

namespace apex::shared::adapters::pg
{

/// RAII transaction guard. Marks connection as poisoned if destroyed without
/// explicit commit() or rollback().
///
/// Template parameter Conn must provide:
///   - awaitable<Result<PgResult>> query_async(string_view sql)
///   - awaitable<Result<PgResult>> query_params_async(string_view sql, span<const string> params)
///   - void mark_poisoned() noexcept
///
/// Usage:
///   PgTransaction txn(conn, pool);
///   co_await txn.execute("INSERT INTO ...");
///   co_await txn.commit();
///   // If commit() is not called before destruction, conn is marked poisoned.
///
/// Thread safety: NOT thread-safe. Same as PgConnection.
template <typename Conn> class PgTransactionT
{
  public:
    PgTransactionT(Conn& conn, PgPool& pool)
        : conn_(conn)
        , pool_(pool)
    {}

    ~PgTransactionT()
    {
        if (begun_ && !finished_)
        {
            conn_.mark_poisoned();
        }
    }

    PgTransactionT(const PgTransactionT&) = delete;
    PgTransactionT& operator=(const PgTransactionT&) = delete;

    /// Begin the transaction (sends "BEGIN" to PostgreSQL).
    /// Must be called before any execute/commit/rollback.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> begin()
    {
        auto result = co_await conn_.query_async("BEGIN");
        if (result)
        {
            begun_ = true;
        }
        co_return result.transform([](auto&&) {});
    }

    /// Execute a SQL statement within this transaction.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>> execute(std::string_view sql)
    {
        if (!begun_)
        {
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
        co_return co_await conn_.query_async(sql);
    }

    /// Execute a parameterized SQL statement within this transaction.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>>
    execute_params(std::string_view sql, std::span<const std::string> params)
    {
        if (!begun_)
        {
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
        co_return co_await conn_.query_params_async(sql, params);
    }

    /// Commit the transaction. Marks as finished on success.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> commit()
    {
        if (!begun_)
        {
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
        auto result = co_await conn_.query_async("COMMIT");
        if (result)
        {
            finished_ = true;
        }
        co_return result.transform([](auto&&) {});
    }

    /// Rollback the transaction. Always marks as finished.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> rollback()
    {
        if (!begun_)
        {
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
        auto result = co_await conn_.query_async("ROLLBACK");
        finished_ = true; // rollback completes regardless of success/failure
        co_return result.transform([](auto&&) {});
    }

  private:
    Conn& conn_;
    PgPool& pool_;
    bool begun_{false};
    bool finished_{false};
};

/// Concrete alias for production use.
using PgTransaction = PgTransactionT<PgConnection>;

} // namespace apex::shared::adapters::pg
