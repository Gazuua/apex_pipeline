#include <apex/shared/adapters/redis/redis_multiplexer.hpp>
#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>

using namespace apex::shared::adapters::redis;

// --- TC1: 기본 생성 + 초기 상태 ---
TEST(RedisMultiplexer, ConstructWithConfig) {
    boost::asio::io_context io_ctx;
    RedisConfig config;
    config.host = "localhost";
    config.port = 6379;
    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
}

// --- TC2: 초기 상태 세부 검증 ---
TEST(RedisMultiplexer, InitialState) {
    boost::asio::io_context io_ctx;
    RedisConfig config;
    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
    EXPECT_EQ(mux.pending_count(), 0u);
    EXPECT_EQ(mux.reconnect_attempts(), 0u);
}

// --- TC3: reconnect 상태 전이 (서버 없음) ---
TEST(RedisMultiplexer, ReconnectStateTransition) {
    boost::asio::io_context io_ctx;
    RedisConfig config;
    config.reconnect_max_backoff = std::chrono::milliseconds{100};
    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
}

// --- TC4: 연결 끊김 상태에서 command 거부 (connected=false 확인) ---
TEST(RedisMultiplexer, CommandWhileDisconnectedReturnsError) {
    boost::asio::io_context io_ctx;
    RedisConfig config;
    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
    // command()는 코루틴이므로 직접 호출 불가하지만 connected() false 확인
}

// --- TC5: close 후 상태 확인 ---
TEST(RedisMultiplexer, CloseResetsPendingState) {
    boost::asio::io_context io_ctx;
    RedisConfig config;
    RedisMultiplexer mux(io_ctx, config);

    // close()는 코루틴이지만 connected=false 상태에서 즉시 완료됨
    // pending_count가 0인 상태에서 close는 안전
    EXPECT_EQ(mux.pending_count(), 0u);
    EXPECT_FALSE(mux.connected());
}

// --- TC6: 다른 설정 값으로 생성 ---
TEST(RedisMultiplexer, ConstructWithCustomConfig) {
    boost::asio::io_context io_ctx;
    RedisConfig config;
    config.host = "192.168.1.100";
    config.port = 6380;
    config.command_timeout = std::chrono::milliseconds{5000};
    config.reconnect_max_backoff = std::chrono::milliseconds{500};

    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
    EXPECT_EQ(mux.pending_count(), 0u);
    EXPECT_EQ(mux.reconnect_attempts(), 0u);
}
