// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>
#include <apex/shared/adapters/pg/pg_result.hpp>

#include <boost/asio/awaitable.hpp>

#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::shared::adapters::pg::mock
{

/// Lightweight mock that satisfies PgTransactionT<Conn> requirements
/// without any libpq or network dependency.
class MockPgConn
{
  public:
    // ---- PgTransactionT Conn interface ----

    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>> query_async(std::string_view sql)
    {
        recorded_queries_.emplace_back(sql);

        if (fail_all_queries_)
        {
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }

        if (result_queue_.empty())
        {
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }

        auto result = std::move(result_queue_.front());
        result_queue_.pop();
        co_return result;
    }

    [[nodiscard]] boost::asio::awaitable<apex::core::Result<PgResult>>
    query_params_async(std::string_view sql, [[maybe_unused]] std::span<const std::string> params)
    {
        co_return co_await query_async(sql);
    }

    void mark_poisoned() noexcept
    {
        poisoned_ = true;
    }

    [[nodiscard]] bool is_poisoned() const noexcept
    {
        return poisoned_;
    }

    // ---- Test API ----

    /// Enqueue a success result (default-constructed PgResult).
    void enqueue_success()
    {
        result_queue_.push(PgResult{});
    }

    /// Enqueue an error result with the given code.
    void enqueue_error(apex::core::ErrorCode ec)
    {
        result_queue_.push(std::unexpected(ec));
    }

    /// All recorded SQL strings in call order.
    [[nodiscard]] const std::vector<std::string>& queries() const noexcept
    {
        return recorded_queries_;
    }

    /// When true, every query_async call returns AdapterError regardless of queue.
    void set_fail_queries(bool fail) noexcept
    {
        fail_all_queries_ = fail;
    }

  private:
    std::queue<apex::core::Result<PgResult>> result_queue_;
    std::vector<std::string> recorded_queries_;
    bool poisoned_{false};
    bool fail_all_queries_{false};
};

} // namespace apex::shared::adapters::pg::mock
