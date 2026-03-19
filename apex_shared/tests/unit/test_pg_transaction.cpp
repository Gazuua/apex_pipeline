#include <apex/shared/adapters/pg/pg_config.hpp>
#include <apex/shared/adapters/pg/pg_connection.hpp>
#include <apex/shared/adapters/pg/pg_transaction.hpp>

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters::pg;

// --- TC1: begin() 미호출 시 소멸자에서 poison 없음 ---
TEST(PgTransaction, DestructorWithoutBegunDoesNotPoison)
{
    boost::asio::io_context io_ctx;
    PgConnection conn(io_ctx);
    EXPECT_FALSE(conn.is_poisoned());

    PgAdapterConfig pg_config;
    pg_config.connection_string = ""; // dummy -- no real DB needed
    PgPool pool(io_ctx, pg_config);
    {
        PgTransaction txn(conn, pool);
        // begin() not called -- destructor should NOT mark poisoned
    }
    EXPECT_FALSE(conn.is_poisoned());
}

// --- TC2: begin()/commit() 정상 흐름은 async DB 필요 ---
TEST(PgTransaction, CommitPreventsPoison)
{
    // begin()/commit() are async coroutines requiring a DB connection.
    GTEST_SKIP() << "commit() requires async DB connection";
}

// --- TC3: begin() 은 async DB 필요 ---
TEST(PgTransaction, BeginRequiresAsyncConnection)
{
    // begin() is an async coroutine that sends "BEGIN" to PostgreSQL.
    GTEST_SKIP() << "begin() requires async DB connection";
}

// --- TC4: 여러 PgTransaction 인스턴스 생성/파괴 안정성 ---
TEST(PgTransaction, MultipleTransactionsOnSameConnection)
{
    boost::asio::io_context io_ctx;
    PgConnection conn(io_ctx);
    PgAdapterConfig pg_config;
    pg_config.connection_string = "";
    PgPool pool(io_ctx, pg_config);

    // 첫 번째 트랜잭션: begin 미호출 → poison 없음
    {
        PgTransaction txn1(conn, pool);
    }
    EXPECT_FALSE(conn.is_poisoned());

    // 두 번째 트랜잭션: 마찬가지로 begin 미호출
    {
        PgTransaction txn2(conn, pool);
    }
    EXPECT_FALSE(conn.is_poisoned());
}

// --- TC5: PgTransaction은 복사/이동 불가 ---
TEST(PgTransaction, NonCopyableNonMovable)
{
    EXPECT_FALSE(std::is_copy_constructible_v<PgTransaction>);
    EXPECT_FALSE(std::is_copy_assignable_v<PgTransaction>);
    EXPECT_FALSE(std::is_move_constructible_v<PgTransaction>);
    EXPECT_FALSE(std::is_move_assignable_v<PgTransaction>);
}
