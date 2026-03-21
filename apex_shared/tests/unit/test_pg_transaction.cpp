// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "mock_pg_connection.hpp"

#include <apex/core/error_code.hpp>
#include <apex/shared/adapters/pg/pg_transaction.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

using namespace apex::shared::adapters::pg;
using apex::core::ErrorCode;
using MockTxn = PgTransactionT<mock::MockPgConn>;

// ---------------------------------------------------------------------------
// Coroutine runner helper — runs an awaitable synchronously on io_context.
// use_future 기반: 코루틴 내부 예외가 future로 전파되어 진단 가능.
// ---------------------------------------------------------------------------
template <typename T> T run_coro(boost::asio::io_context& io, boost::asio::awaitable<T> aw)
{
    auto future = boost::asio::co_spawn(io, std::move(aw), boost::asio::use_future);
    io.run();
    io.restart();
    return future.get();
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class PgTransactionTest : public ::testing::Test
{
  protected:
    boost::asio::io_context io_;
    mock::MockPgConn conn_;
};

// ---------------------------------------------------------------------------
// TC1: begin() 성공 → execute/commit 동작 확인
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, BeginSuccess_SetsBegun)
{
    // begin → execute → commit 각각 query_async 1회씩 = 3회
    conn_.enqueue_success(); // BEGIN
    conn_.enqueue_success(); // execute
    conn_.enqueue_success(); // COMMIT

    MockTxn txn(conn_);

    auto begin_res = run_coro(io_, txn.begin());
    ASSERT_TRUE(begin_res.has_value());

    auto exec_res = run_coro(io_, txn.execute("INSERT INTO t VALUES (1)"));
    ASSERT_TRUE(exec_res.has_value());

    auto commit_res = run_coro(io_, txn.commit());
    ASSERT_TRUE(commit_res.has_value());

    // Verify recorded SQL
    ASSERT_EQ(conn_.queries().size(), 3u);
    EXPECT_EQ(conn_.queries()[0], "BEGIN");
    EXPECT_EQ(conn_.queries()[1], "INSERT INTO t VALUES (1)");
    EXPECT_EQ(conn_.queries()[2], "COMMIT");
    EXPECT_FALSE(conn_.is_poisoned());
}

// ---------------------------------------------------------------------------
// TC2: begin() 미호출 → commit() = AdapterError
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, CommitWithoutBegin_ReturnsError)
{
    MockTxn txn(conn_);
    auto res = run_coro(io_, txn.commit());
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), ErrorCode::AdapterError);
}

// ---------------------------------------------------------------------------
// TC3: begin() 미호출 → rollback() = AdapterError
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, RollbackWithoutBegin_ReturnsError)
{
    MockTxn txn(conn_);
    auto res = run_coro(io_, txn.rollback());
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), ErrorCode::AdapterError);
}

// ---------------------------------------------------------------------------
// TC4: commit 성공 → finished 설정 → 소멸자에서 poison 없음
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, CommitSuccess_SetsFinished)
{
    conn_.enqueue_success(); // BEGIN
    conn_.enqueue_success(); // COMMIT

    {
        MockTxn txn(conn_);
        auto begin_res = run_coro(io_, txn.begin());
        ASSERT_TRUE(begin_res.has_value());

        auto commit_res = run_coro(io_, txn.commit());
        ASSERT_TRUE(commit_res.has_value());
    }
    // Destructor should NOT poison because commit succeeded
    EXPECT_FALSE(conn_.is_poisoned());
}

// ---------------------------------------------------------------------------
// TC5: rollback 실패해도 항상 finished 설정 → poison 없음
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, RollbackAlwaysSetsFinished)
{
    conn_.enqueue_success(); // BEGIN

    {
        MockTxn txn(conn_);
        auto begin_res = run_coro(io_, txn.begin());
        ASSERT_TRUE(begin_res.has_value());

        // rollback will fail (no result in queue), but finished_ should still be set
        auto rb_res = run_coro(io_, txn.rollback());
        EXPECT_FALSE(rb_res.has_value());
    }
    // Despite rollback failure, destructor must NOT poison (finished_ = true)
    EXPECT_FALSE(conn_.is_poisoned());
}

// ---------------------------------------------------------------------------
// TC6: begin() 후 commit/rollback 없이 소멸 → poison
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, DestructorPoisons_UnfinishedTxn)
{
    conn_.enqueue_success(); // BEGIN

    {
        MockTxn txn(conn_);
        auto begin_res = run_coro(io_, txn.begin());
        ASSERT_TRUE(begin_res.has_value());
        // No commit or rollback — destructor should poison
    }
    EXPECT_TRUE(conn_.is_poisoned());
}

// ---------------------------------------------------------------------------
// TC7: begin() 미호출 → 소멸 → poison 없음
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, DestructorSafe_NotBegun)
{
    {
        MockTxn txn(conn_);
        // No begin() — destructor should NOT poison
    }
    EXPECT_FALSE(conn_.is_poisoned());
}

// ---------------------------------------------------------------------------
// TC8: commit 후 execute → AdapterError (finished_ 상태에서 쿼리 차단)
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, ExecuteAfterCommit_ReturnsError)
{
    conn_.enqueue_success(); // BEGIN
    conn_.enqueue_success(); // COMMIT

    MockTxn txn(conn_);

    auto begin_res = run_coro(io_, txn.begin());
    ASSERT_TRUE(begin_res.has_value());

    auto commit_res = run_coro(io_, txn.commit());
    ASSERT_TRUE(commit_res.has_value());

    // finished_ == true → execute must return AdapterError
    auto exec_res = run_coro(io_, txn.execute("SELECT 1"));
    ASSERT_FALSE(exec_res.has_value());
    EXPECT_EQ(exec_res.error(), ErrorCode::AdapterError);
}

// ---------------------------------------------------------------------------
// TC9: execute() 미시작 상태 호출 → AdapterError
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, ExecuteWithoutBegin_ReturnsError)
{
    MockTxn txn(conn_);
    auto res = run_coro(io_, txn.execute("SELECT 1"));
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), ErrorCode::AdapterError);
}

// ---------------------------------------------------------------------------
// TC10: execute_params() 미시작 상태 → AdapterError
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, ExecuteParamsWithoutBegin_ReturnsError)
{
    MockTxn txn(conn_);
    std::vector<std::string> params = {"value1"};
    auto res = run_coro(io_, txn.execute_params("SELECT $1", params));
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), ErrorCode::AdapterError);
}

// ---------------------------------------------------------------------------
// TC11: execute_params() 정상 경로 — begin → execute_params → commit
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, ExecuteParamsSuccess)
{
    conn_.enqueue_success(); // BEGIN
    conn_.enqueue_success(); // execute_params
    conn_.enqueue_success(); // COMMIT

    MockTxn txn(conn_);

    auto begin_res = run_coro(io_, txn.begin());
    ASSERT_TRUE(begin_res.has_value());

    std::vector<std::string> params = {"42", "hello"};
    auto exec_res = run_coro(io_, txn.execute_params("INSERT INTO t VALUES ($1, $2)", params));
    ASSERT_TRUE(exec_res.has_value());

    auto commit_res = run_coro(io_, txn.commit());
    ASSERT_TRUE(commit_res.has_value());

    ASSERT_EQ(conn_.queries().size(), 3u);
    EXPECT_EQ(conn_.queries()[0], "BEGIN");
    EXPECT_EQ(conn_.queries()[1], "INSERT INTO t VALUES ($1, $2)");
    EXPECT_EQ(conn_.queries()[2], "COMMIT");
}

// ---------------------------------------------------------------------------
// TC12: begin() 실패 → begun_ false 유지 → 소멸자 poison 없음
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, BeginFailure_DoesNotSetBegun)
{
    // result_queue_ 비어 있음 → begin 실패
    {
        MockTxn txn(conn_);
        auto begin_res = run_coro(io_, txn.begin());
        ASSERT_FALSE(begin_res.has_value());
        EXPECT_EQ(begin_res.error(), ErrorCode::AdapterError);
    }
    // begun_ false이므로 소멸자가 poison 안 함
    EXPECT_FALSE(conn_.is_poisoned());
}

// ---------------------------------------------------------------------------
// TC13: commit() 실패 → finished_ false 유지 → 소멸자가 poison
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, CommitFailure_DestuctorPoisons)
{
    conn_.enqueue_success(); // BEGIN
    // COMMIT용 결과 없음 → commit 실패

    {
        MockTxn txn(conn_);
        auto begin_res = run_coro(io_, txn.begin());
        ASSERT_TRUE(begin_res.has_value());

        auto commit_res = run_coro(io_, txn.commit());
        ASSERT_FALSE(commit_res.has_value());
    }
    // commit 실패 → finished_ false → 소멸자가 poison
    EXPECT_TRUE(conn_.is_poisoned());
}
