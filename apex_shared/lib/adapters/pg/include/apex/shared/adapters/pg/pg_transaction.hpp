// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

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
/// Usage:
///   PgTransaction txn(conn, pool);
///   co_await txn.execute("INSERT INTO ...");
///   co_await txn.commit();
///   // If commit() is not called before destruction, conn is marked poisoned.
///
/// Thread safety: NOT thread-safe. Same as PgConnection.
class PgTransaction
{
  public:
    PgTransaction(PgConnection& conn, PgPool& pool);
    ~PgTransaction();

    PgTransaction(const PgTransaction&) = delete;
    PgTransaction& operator=(const PgTransaction&) = delete;

    /// Begin the transaction (sends "BEGIN" to PostgreSQL).
    /// Must be called before any execute/commit/rollback.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> begin();

    /// Execute a SQL statement within this transaction.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>> execute(std::string_view sql);

    /// Execute a parameterized SQL statement within this transaction.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>>
    execute_params(std::string_view sql, std::span<const std::string> params);

    /// Commit the transaction. Marks as finished on success.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> commit();

    /// Rollback the transaction. Always marks as finished.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> rollback();

  private:
    PgConnection& conn_;
    PgPool& pool_;
    bool begun_{false};
    bool finished_{false};
};

} // namespace apex::shared::adapters::pg
