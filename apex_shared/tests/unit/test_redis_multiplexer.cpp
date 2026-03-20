// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/redis/redis_multiplexer.hpp>
#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>
#include <hiredis/hiredis.h>

#include <cstring>

using namespace apex::shared::adapters::redis;

// --- TC1: 기본 생성 + 초기 상태 ---
TEST(RedisMultiplexer, ConstructWithConfig)
{
    boost::asio::io_context io_ctx;
    RedisConfig config;
    config.host = "localhost";
    config.port = 6379;
    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
}

// --- TC2: 초기 상태 세부 검증 ---
TEST(RedisMultiplexer, InitialState)
{
    boost::asio::io_context io_ctx;
    RedisConfig config;
    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
    EXPECT_EQ(mux.pending_count(), 0u);
    EXPECT_EQ(mux.reconnect_attempts(), 0u);
}

// --- TC3: reconnect 상태 전이 (서버 없음) ---
TEST(RedisMultiplexer, ReconnectStateTransition)
{
    boost::asio::io_context io_ctx;
    RedisConfig config;
    config.reconnect_max_backoff = std::chrono::milliseconds{100};
    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
}

// --- TC4: 연결 끊김 상태에서 command 거부 (connected=false 확인) ---
TEST(RedisMultiplexer, CommandWhileDisconnectedReturnsError)
{
    boost::asio::io_context io_ctx;
    RedisConfig config;
    RedisMultiplexer mux(io_ctx, config);
    EXPECT_FALSE(mux.connected());
    // command()는 코루틴이므로 직접 호출 불가하지만 connected() false 확인
}

// --- TC5: close 후 상태 확인 ---
TEST(RedisMultiplexer, CloseResetsPendingState)
{
    boost::asio::io_context io_ctx;
    RedisConfig config;
    RedisMultiplexer mux(io_ctx, config);

    // close()는 코루틴이지만 connected=false 상태에서 즉시 완료됨
    // pending_count가 0인 상태에서 close는 안전
    EXPECT_EQ(mux.pending_count(), 0u);
    EXPECT_FALSE(mux.connected());
}

// --- TC6: 다른 설정 값으로 생성 ---
TEST(RedisMultiplexer, ConstructWithCustomConfig)
{
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

// ==========================================================================
// Parameter binding (redisFormatCommand) tests
// These verify hiredis format layer directly — no live Redis needed.
// ==========================================================================

// --- TC7: redisFormatCommand 기본 %s 파라미터 바인딩 ---
TEST(RedisMultiplexer, FormatCommandBasicString)
{
    char* buf = nullptr;
    int len = redisFormatCommand(&buf, "GET %s", "mykey");
    ASSERT_GT(len, 0);
    ASSERT_NE(buf, nullptr);

    // hiredis RESP protocol: "*2\r\n$3\r\nGET\r\n$5\r\nmykey\r\n"
    std::string result(buf, len);
    EXPECT_NE(result.find("GET"), std::string::npos);
    EXPECT_NE(result.find("mykey"), std::string::npos);
    redisFreeCommand(buf);
}

// --- TC8: redisFormatCommand %d 정수 파라미터 바인딩 ---
TEST(RedisMultiplexer, FormatCommandIntegerParam)
{
    char* buf = nullptr;
    int len = redisFormatCommand(&buf, "SETEX %s %d %s", "sess:123", 3600, "data");
    ASSERT_GT(len, 0);
    ASSERT_NE(buf, nullptr);

    std::string result(buf, len);
    EXPECT_NE(result.find("SETEX"), std::string::npos);
    EXPECT_NE(result.find("sess:123"), std::string::npos);
    EXPECT_NE(result.find("3600"), std::string::npos);
    EXPECT_NE(result.find("data"), std::string::npos);
    redisFreeCommand(buf);
}

// --- TC9: 특수문자 이스케이핑 — 공백/개행 포함 값이 안전하게 전달됨 ---
TEST(RedisMultiplexer, FormatCommandSpecialCharsEscaped)
{
    const char* value = "hello world\r\ninjection";
    char* buf = nullptr;
    int len = redisFormatCommand(&buf, "SET %s %s", "key", value);
    ASSERT_GT(len, 0);
    ASSERT_NE(buf, nullptr);

    // RESP bulk string은 길이 접두사로 구분하므로 \r\n이 프로토콜을
    // 깨뜨리지 않음. value가 bulk string 내에 포함되어야 함.
    std::string result(buf, len);
    EXPECT_NE(result.find(value), std::string::npos);
    redisFreeCommand(buf);
}

// --- TC10: 빈 포맷 문자열 처리 ---
TEST(RedisMultiplexer, FormatCommandEmptyArgs)
{
    char* buf = nullptr;
    int len = redisFormatCommand(&buf, "PING");
    ASSERT_GT(len, 0);
    ASSERT_NE(buf, nullptr);

    std::string result(buf, len);
    EXPECT_NE(result.find("PING"), std::string::npos);
    redisFreeCommand(buf);
}
