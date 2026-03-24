// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/core/scoped_logger.hpp>
#include <apex/shared/adapters/pg/pg_connection.hpp>
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
///   PgTransaction txn(conn);
///   co_await txn.begin();
///   co_await txn.execute("INSERT INTO ...");
///   co_await txn.commit();
///   // If commit() is not called before destruction, conn is marked poisoned.
///
/// Thread safety: NOT thread-safe. Same as PgConnection.
template <typename Conn> class PgTransactionT
{
  public:
    explicit PgTransactionT(Conn& conn)
        : conn_(conn)
    {}

    ~PgTransactionT()
    {
        if (begun_ && !finished_)
        {
            logger_.warn("destroyed without commit/rollback — poisoning connection");
            conn_.mark_poisoned();
        }
    }

    PgTransactionT(const PgTransactionT&) = delete;
    PgTransactionT& operator=(const PgTransactionT&) = delete;

    /// Begin the transaction (sends "BEGIN" to PostgreSQL).
    /// Must be called before any execute/commit/rollback.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> begin()
    {
        logger_.debug("BEGIN");
        auto result = co_await conn_.query_async("BEGIN");
        if (result)
        {
            begun_ = true;
        }
        else
        {
            logger_.error("BEGIN failed");
        }
        co_return result.transform([](auto&&) {});
    }

    /// Execute a SQL statement within this transaction.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>> execute(std::string_view sql)
    {
        if (!begun_ || finished_)
        {
            logger_.error("execute called on inactive txn (begun={}, finished={})", begun_, finished_);
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
        logger_.trace("execute sql_len={}", sql.size());
        co_return co_await conn_.query_async(sql);
    }

    /// Execute a parameterized SQL statement within this transaction.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>>
    execute_params(std::string_view sql, std::span<const std::string> params)
    {
        if (!begun_ || finished_)
        {
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
        co_return co_await conn_.query_params_async(sql, params);
    }

    /// Commit the transaction. Marks as finished on success.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> commit()
    {
        if (!begun_ || finished_)
        {
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
        logger_.debug("COMMIT");
        auto result = co_await conn_.query_async("COMMIT");
        if (result)
        {
            finished_ = true;
        }
        else
        {
            logger_.error("COMMIT failed");
        }
        co_return result.transform([](auto&&) {});
    }

    /// Rollback the transaction. Always marks as finished.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> rollback()
    {
        if (!begun_ || finished_)
        {
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
        logger_.debug("ROLLBACK");
        auto result = co_await conn_.query_async("ROLLBACK");
        finished_ = true; // rollback completes regardless of success/failure
        if (!result)
        {
            logger_.error("ROLLBACK failed");
        }
        co_return result.transform([](auto&&) {});
    }

  private:
    Conn& conn_;
    bool begun_{false};
    bool finished_{false};
    apex::core::ScopedLogger logger_{"PgTransaction", apex::core::ScopedLogger::NO_CORE, "app"};
};

/// Concrete alias for production use.
using PgTransaction = PgTransactionT<PgConnection>;

} // namespace apex::shared::adapters::pg
