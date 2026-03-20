// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/pg/pg_config.hpp>
#include <apex/shared/adapters/pg/pg_connection.hpp>
#include <apex/shared/adapters/pg/pg_pool.hpp>
#include <apex/shared/adapters/pg/pg_result.hpp>

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

using namespace apex::shared::adapters::pg;

class PgRoundtripTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        config_.connection_string = "host=localhost port=6432 dbname=apex user=apex password=apex";
    }
    PgAdapterConfig config_;
    boost::asio::io_context io_ctx_;
};

TEST_F(PgRoundtripTest, SimpleQuery)
{
    bool test_passed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            PgPool pool(io_ctx_, config_);
            auto conn_result = co_await pool.acquire_connected();
            EXPECT_TRUE(conn_result.has_value()) << "PG connect failed - is PgBouncer running on localhost:6432?";
            if (!conn_result.has_value())
                co_return;
            auto conn = std::move(conn_result.value());

            auto result = co_await conn->query_async("SELECT 1 AS value");
            EXPECT_TRUE(result.has_value());
            EXPECT_TRUE(result->ok());
            EXPECT_EQ(result->row_count(), 1);
            EXPECT_EQ(result->value(0, 0), "1");

            pool.release(std::move(conn));
            test_passed = true;
            co_return;
        },
        boost::asio::detached);
    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}

TEST_F(PgRoundtripTest, CreateInsertSelectDelete)
{
    bool test_passed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            PgPool pool(io_ctx_, config_);
            auto conn_result = co_await pool.acquire_connected();
            EXPECT_TRUE(conn_result.has_value());
            if (!conn_result.has_value())
                co_return;
            auto conn = std::move(conn_result.value());

            // CREATE TABLE
            auto create_result = co_await conn->execute_async("CREATE TABLE IF NOT EXISTS integration_test ("
                                                              "  id SERIAL PRIMARY KEY,"
                                                              "  name TEXT NOT NULL,"
                                                              "  created_at TIMESTAMPTZ DEFAULT NOW()"
                                                              ")");
            EXPECT_TRUE(create_result.has_value());

            // INSERT
            auto insert_result =
                co_await conn->execute_async("INSERT INTO integration_test (name) VALUES ('apex-test')");
            EXPECT_TRUE(insert_result.has_value());

            // SELECT
            auto select_result =
                co_await conn->query_async("SELECT name FROM integration_test WHERE name = 'apex-test' LIMIT 1");
            EXPECT_TRUE(select_result.has_value());
            EXPECT_TRUE(select_result->ok());
            EXPECT_GE(select_result->row_count(), 1);
            EXPECT_EQ(select_result->value(0, 0), "apex-test");

            // CLEANUP
            (void)co_await conn->execute_async("DROP TABLE IF EXISTS integration_test");

            pool.release(std::move(conn));
            test_passed = true;
            co_return;
        },
        boost::asio::detached);
    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}

TEST_F(PgRoundtripTest, PoolStats)
{
    bool test_passed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            PgPool pool(io_ctx_, config_);
            auto conn_result = co_await pool.acquire_connected();
            EXPECT_TRUE(conn_result.has_value());
            if (!conn_result.has_value())
                co_return;
            auto conn = std::move(conn_result.value());

            auto result = co_await conn->query_async("SELECT 1");
            EXPECT_TRUE(result.has_value());

            pool.release(std::move(conn));

            // Verify PoolStats
            const auto& stats = pool.stats();
            EXPECT_GE(stats.total_acquired, 1u);
            EXPECT_GE(stats.total_released, 1u);
            EXPECT_GE(stats.total_created, 1u);

            test_passed = true;
            co_return;
        },
        boost::asio::detached);
    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}
