#include <apex/shared/adapters/pg/pg_transaction.hpp>
#include <apex/shared/adapters/pg/pg_connection.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters::pg;

TEST(PgTransaction, DestructorWithoutBegunDoesNotPoison) {
    boost::asio::io_context io_ctx;
    PgConnection conn(io_ctx);
    EXPECT_FALSE(conn.is_poisoned());

    PgAdapterConfig pg_config;
    pg_config.connection_string = "";  // dummy — no real DB needed
    PgPool pool(io_ctx, pg_config);
    {
        PgTransaction txn(conn, pool);
        // begin() not called — destructor should NOT mark poisoned
    }
    EXPECT_FALSE(conn.is_poisoned());
}

TEST(PgTransaction, CommitPreventsPoison) {
    // begin()/commit() are async coroutines requiring a DB connection.
    GTEST_SKIP() << "commit() requires async DB connection";
}

TEST(PgTransaction, BeginRequiresAsyncConnection) {
    // begin() is an async coroutine that sends "BEGIN" to PostgreSQL.
    GTEST_SKIP() << "begin() requires async DB connection";
}
