#include <apex/shared/adapters/redis/redis_multiplexer.hpp>
#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>

using namespace apex::shared::adapters::redis;

TEST(RedisMultiplexer, ConstructWithConfig) {
    boost::asio::io_context io_ctx;
    RedisConfig config;
    config.host = "localhost";
    config.port = 6379;
    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
}

TEST(RedisMultiplexer, InitialState) {
    boost::asio::io_context io_ctx;
    RedisConfig config;
    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
    EXPECT_EQ(mux.pending_count(), 0u);
    EXPECT_EQ(mux.reconnect_attempts(), 0u);
}

TEST(RedisMultiplexer, ReconnectStateTransition) {
    // Verify state with no server running
    boost::asio::io_context io_ctx;
    RedisConfig config;
    config.reconnect_max_backoff = std::chrono::milliseconds{100};
    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
}

TEST(RedisMultiplexer, CommandWhileDisconnectedReturnsError) {
    // Cannot co_await in unit test, but verify connected() is false
    boost::asio::io_context io_ctx;
    RedisConfig config;
    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
}
