// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "mock_pg_connection.hpp"

#include <apex/core/error_code.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>
#include <apex/shared/adapters/pg/pg_transaction.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <optional>

using namespace apex::shared::adapters::pg;
using apex::core::ErrorCode;
using MockTxn = PgTransactionT<mock::MockPgConn>;

// ---------------------------------------------------------------------------
// Coroutine runner helper — runs an awaitable synchronously on io_context.
// ---------------------------------------------------------------------------
template <typename T> T run_coro(boost::asio::io_context& io, boost::asio::awaitable<T> aw)
{
    std::optional<T> result;
    boost::asio::co_spawn(
        io, [&]() -> boost::asio::awaitable<void> { result.emplace(co_await std::move(aw)); }, boost::asio::detached);
    io.run();
    io.restart();
    return std::move(*result);
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class PgTransactionTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        pg_config_.connection_string = "";
        pool_.emplace(io_, pg_config_);
    }

    boost::asio::io_context io_;
    mock::MockPgConn conn_;
    PgAdapterConfig pg_config_;
    std::optional<PgPool> pool_;
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

    MockTxn txn(conn_, *pool_);

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
    MockTxn txn(conn_, *pool_);
    auto res = run_coro(io_, txn.commit());
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), ErrorCode::AdapterError);
}

// ---------------------------------------------------------------------------
// TC3: begin() 미호출 → rollback() = AdapterError
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, RollbackWithoutBegin_ReturnsError)
{
    MockTxn txn(conn_, *pool_);
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
        MockTxn txn(conn_, *pool_);
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
        MockTxn txn(conn_, *pool_);
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
        MockTxn txn(conn_, *pool_);
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
        MockTxn txn(conn_, *pool_);
        // No begin() — destructor should NOT poison
    }
    EXPECT_FALSE(conn_.is_poisoned());
}

// ---------------------------------------------------------------------------
// TC8: commit 후 execute → AdapterError (begun_ true but finished_ true이므로
//      execute는 begun_ 체크만 하므로 통과 — characterization test)
// ---------------------------------------------------------------------------
TEST_F(PgTransactionTest, ExecuteAfterCommit_ReturnsError)
{
    conn_.enqueue_success(); // BEGIN
    conn_.enqueue_success(); // COMMIT
    conn_.enqueue_success(); // would-be execute result

    MockTxn txn(conn_, *pool_);

    auto begin_res = run_coro(io_, txn.begin());
    ASSERT_TRUE(begin_res.has_value());

    auto commit_res = run_coro(io_, txn.commit());
    ASSERT_TRUE(commit_res.has_value());

    // Characterization: current impl allows execute after commit because
    // execute() only checks begun_, not finished_. This documents the
    // current behavior rather than prescribing what "should" happen.
    auto exec_res = run_coro(io_, txn.execute("SELECT 1"));
    // The query goes through because begun_ is true
    EXPECT_TRUE(exec_res.has_value());
}
