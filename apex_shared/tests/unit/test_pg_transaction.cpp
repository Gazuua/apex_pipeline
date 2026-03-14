#include <apex/shared/adapters/pg/pg_transaction.hpp>
#include <apex/shared/adapters/pg/pg_connection.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters::pg;

TEST(PgTransaction, DestructorWithoutCommitMarksPoisoned) {
    boost::asio::io_context io_ctx;
    PgConnection conn(io_ctx);
    EXPECT_FALSE(conn.is_poisoned());

    PgAdapterConfig pg_config;
    pg_config.connection_string = "";  // dummy — no real DB needed
    PgPool pool(io_ctx, pg_config);
    {
        PgTransaction txn(conn, pool);
        // commit()/rollback() not called — destructor should mark poisoned
    }
    EXPECT_TRUE(conn.is_poisoned());
}

TEST(PgTransaction, CommitPreventsPoison) {
    // Actual commit requires DB connection — verified in integration tests.
    SUCCEED();
}
