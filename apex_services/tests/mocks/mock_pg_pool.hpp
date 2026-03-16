#pragma once

/// Mock PgPool — query 기록 + 미리 설정된 결과 반환.
/// 실제 PostgreSQL 연결 없이 DB 상호작용을 검증.

#include <apex/core/result.hpp>

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace apex::test {

/// Mock PG query result row.
using MockPgRow = std::vector<std::string>;

/// Mock PG query result.
struct MockPgResult {
    std::vector<std::string> columns;
    std::vector<MockPgRow> rows;
    int affected_rows{0};
};

/// Recorded PG query.
struct PgQuery {
    std::string sql;
    std::vector<std::string> params;
};

/// Mock PgPool.
/// Records all queries and returns pre-configured results.
/// Synchronous API for unit test simplicity.
class MockPgPool {
public:
    MockPgPool() = default;

    // --- Query recording ---

    /// Execute query with no params.
    [[nodiscard]] apex::core::Result<MockPgResult>
    query(std::string_view sql)
    {
        return query(sql, {});
    }

    /// Execute parameterized query.
    [[nodiscard]] apex::core::Result<MockPgResult>
    query(std::string_view sql, std::vector<std::string> params)
    {
        if (fail_queries_) {
            return apex::core::error(apex::core::ErrorCode::AdapterError);
        }
        std::lock_guard lock(mu_);
        queries_.push_back(PgQuery{
            .sql = std::string(sql),
            .params = std::move(params),
        });

        if (!result_queue_.empty()) {
            auto result = std::move(result_queue_.front());
            result_queue_.pop_front();
            return result;
        }
        return MockPgResult{};
    }

    /// Execute command (INSERT/UPDATE/DELETE).
    [[nodiscard]] apex::core::Result<int>
    execute(std::string_view sql)
    {
        return execute(sql, {});
    }

    /// Execute parameterized command.
    [[nodiscard]] apex::core::Result<int>
    execute(std::string_view sql, std::vector<std::string> params)
    {
        if (fail_queries_) {
            return apex::core::error(apex::core::ErrorCode::AdapterError);
        }
        std::lock_guard lock(mu_);
        queries_.push_back(PgQuery{
            .sql = std::string(sql),
            .params = std::move(params),
        });

        if (!affected_rows_queue_.empty()) {
            auto rows = affected_rows_queue_.front();
            affected_rows_queue_.pop_front();
            return rows;
        }
        return 0;
    }

    // --- Result pre-configuration ---

    /// Enqueue a query result.
    void enqueue_result(MockPgResult result) {
        std::lock_guard lock(mu_);
        result_queue_.push_back(std::move(result));
    }

    /// Enqueue affected rows for execute.
    void enqueue_affected_rows(int rows) {
        std::lock_guard lock(mu_);
        affected_rows_queue_.push_back(rows);
    }

    // --- Test inspection ---

    [[nodiscard]] const std::vector<PgQuery>& queries() const {
        return queries_;
    }

    [[nodiscard]] size_t query_count() const {
        std::lock_guard lock(mu_);
        return queries_.size();
    }

    void clear() {
        std::lock_guard lock(mu_);
        queries_.clear();
        result_queue_.clear();
        affected_rows_queue_.clear();
    }

    /// Set all queries to fail with AdapterError.
    void set_fail_queries(bool fail) { fail_queries_ = fail; }

private:
    mutable std::mutex mu_;
    std::vector<PgQuery> queries_;
    std::deque<MockPgResult> result_queue_;
    std::deque<int> affected_rows_queue_;
    bool fail_queries_{false};
};

} // namespace apex::test
