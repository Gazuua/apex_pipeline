#include <apex/shared/adapters/redis/redis_pool.hpp>
#include <apex/shared/adapters/redis/redis_connection.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/adapters/connection_pool.hpp>

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>

using namespace apex::shared::adapters::redis;
using namespace apex::shared::adapters;

class RedisRoundtripTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.host = "localhost";
        config_.port = 6379;
    }
    RedisConfig config_;
    boost::asio::io_context io_ctx_;
};

TEST_F(RedisRoundtripTest, SetAndGet) {
    bool test_passed = false;
    boost::asio::co_spawn(io_ctx_, [&]() -> boost::asio::awaitable<void> {
        RedisPool pool(io_ctx_, config_, PoolConfig{.min_size = 1, .max_size = 4});

        // acquire connection
        auto conn_result = pool.acquire();
        EXPECT_TRUE(conn_result.has_value())
            << "Redis pool acquire failed - is Redis running on localhost:6379?";
        if (!conn_result.has_value()) co_return;

        auto& conn = conn_result.value();

        // SET
        auto [ec_set, reply_set] = co_await conn->async_command(
            "SET test:integration:key hello-redis", boost::asio::use_awaitable);
        EXPECT_FALSE(ec_set);
        EXPECT_FALSE(RedisConnection::is_error_reply(reply_set));

        // GET
        auto [ec_get, reply_get] = co_await conn->async_command(
            "GET test:integration:key", boost::asio::use_awaitable);
        EXPECT_FALSE(ec_get);
        auto val = RedisConnection::parse_string_reply(reply_get);
        EXPECT_TRUE(val.has_value());
        EXPECT_EQ(val.value(), "hello-redis");

        // DEL
        auto [ec_del, reply_del] = co_await conn->async_command(
            "DEL test:integration:key", boost::asio::use_awaitable);
        EXPECT_FALSE(ec_del);
        auto del_count = RedisConnection::parse_integer_reply(reply_del);
        EXPECT_TRUE(del_count.has_value());
        EXPECT_EQ(del_count.value(), 1);

        // GET after DEL
        auto [ec_get2, reply_get2] = co_await conn->async_command(
            "GET test:integration:key", boost::asio::use_awaitable);
        EXPECT_FALSE(ec_get2);
        auto val2 = RedisConnection::parse_string_reply(reply_get2);
        EXPECT_FALSE(val2.has_value());  // nullopt

        pool.release(std::move(conn));
        test_passed = true;
        co_return;
    }, boost::asio::detached);
    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}

TEST_F(RedisRoundtripTest, PoolStats) {
    bool test_passed = false;
    boost::asio::co_spawn(io_ctx_, [&]() -> boost::asio::awaitable<void> {
        RedisPool pool(io_ctx_, config_, PoolConfig{.min_size = 1, .max_size = 4});

        auto conn_result = pool.acquire();
        EXPECT_TRUE(conn_result.has_value());
        if (!conn_result.has_value()) co_return;

        auto& conn = conn_result.value();

        // SET for stats test
        auto [ec, reply] = co_await conn->async_command(
            "SET test:integration:stats ok", boost::asio::use_awaitable);
        EXPECT_FALSE(ec);

        pool.release(std::move(conn));

        // Verify PoolStats
        const auto& stats = pool.stats();
        EXPECT_GE(stats.total_acquired, 1u);
        EXPECT_GE(stats.total_released, 1u);
        EXPECT_GE(stats.total_created, 1u);

        // Cleanup
        auto conn2 = pool.acquire();
        if (conn2.has_value()) {
            (void)co_await conn2.value()->async_command(
                "DEL test:integration:stats", boost::asio::use_awaitable);
            pool.release(std::move(conn2.value()));
        }

        test_passed = true;
        co_return;
    }, boost::asio::detached);
    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}
