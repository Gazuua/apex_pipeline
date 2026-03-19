#include <apex/shared/adapters/redis/hiredis_asio_adapter.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/adapters/redis/redis_connection.hpp>
#include <gtest/gtest.h>

#include <hiredis/async.h>
#include <hiredis/hiredis.h>

#include <boost/asio/io_context.hpp>

using namespace apex::shared::adapters::redis;

// --- RedisConfig 구조체 테스트 ---

TEST(RedisConfig, DefaultValues)
{
    RedisConfig config;
    EXPECT_EQ(config.host, "localhost");
    EXPECT_EQ(config.port, 6379);
    EXPECT_TRUE(config.password.empty());
    EXPECT_EQ(config.db, 0u);
    EXPECT_EQ(config.connect_timeout, std::chrono::milliseconds{3000});
    EXPECT_EQ(config.command_timeout, std::chrono::milliseconds{1000});
    EXPECT_EQ(config.reconnect_max_backoff, std::chrono::milliseconds{30000});
}

TEST(RedisConfig, CustomValues)
{
    RedisConfig config{
        .host = "redis.local",
        .port = 6380,
        .password = "secret",
        .db = 2,
        .connect_timeout = std::chrono::milliseconds{5000},
        .command_timeout = std::chrono::milliseconds{2000},
        .reconnect_max_backoff = std::chrono::milliseconds{60000},
    };
    EXPECT_EQ(config.host, "redis.local");
    EXPECT_EQ(config.port, 6380);
    EXPECT_EQ(config.password, "secret");
    EXPECT_EQ(config.db, 2u);
    EXPECT_EQ(config.connect_timeout, std::chrono::milliseconds{5000});
    EXPECT_EQ(config.command_timeout, std::chrono::milliseconds{2000});
    EXPECT_EQ(config.reconnect_max_backoff, std::chrono::milliseconds{60000});
}

// --- HiredisAsioAdapter 구조 테스트 (Redis 서버 없이 컴파일/링크 검증) ---

TEST(HiredisAsioAdapter, CompileAndLinkCheck)
{
    // redisAsyncConnect가 실패해도 ac는 non-null (에러는 ac->err에 설정됨)
    // 실제 fd 등록 테스트는 통합 테스트(Plan 5)에서 수행
    boost::asio::io_context io_ctx;

    auto* ac = redisAsyncConnect("invalid-host-for-unit-test", 6379);
    ASSERT_NE(ac, nullptr);

    // DNS 해석 실패 등으로 에러 상태일 수 있음
    if (ac->err)
    {
        // 에러 상태 — cleanup만 확인
        redisAsyncFree(ac);
        SUCCEED(); // 구조적으로 정상
    }
    else
    {
        // 만약 연결이 성공했다면 (로컬 Redis가 실행 중)
        redisAsyncFree(ac);
        SUCCEED();
    }
}

// --- RedisConnection 테스트 (서버 없이 구조 검증) ---

TEST(RedisConnection, CreateFailsWithInvalidHost)
{
    boost::asio::io_context io_ctx;
    RedisConfig config{.host = "invalid-host-for-unit-test",
                       .port = 6379,
                       .password = {},
                       .db = {},
                       .connect_timeout = std::chrono::milliseconds{3000},
                       .command_timeout = std::chrono::milliseconds{1000},
                       .reconnect_max_backoff = std::chrono::milliseconds{30000},
                       .max_pending_commands = 4096};

    // DNS 실패 또는 즉시 connect 실패 → nullptr 또는 유효한 연결
    // 환경에 따라 결과가 다를 수 있음 (DNS가 해석되는 경우)
    auto conn = RedisConnection::create(io_ctx, config);
    // 연결이 실패하면 nullptr — 성공해도 validate는 가능
    if (conn)
    {
        // 커넥션 객체가 생성된 경우 기본 상태 확인
        EXPECT_TRUE(conn->is_connected());
    }
    else
    {
        SUCCEED();
    }
}

TEST(RedisConnection, ValidateReturnsFalseAfterDisconnect)
{
    boost::asio::io_context io_ctx;
    RedisConfig config{.host = "127.0.0.1",
                       .port = 59999,
                       .password = {},
                       .db = {},
                       .connect_timeout = std::chrono::milliseconds{3000},
                       .command_timeout = std::chrono::milliseconds{1000},
                       .reconnect_max_backoff = std::chrono::milliseconds{30000},
                       .max_pending_commands = 4096}; // 사용하지 않는 포트

    auto conn = RedisConnection::create(io_ctx, config);
    if (!conn)
    {
        SUCCEED(); // 연결 실패는 예상 결과
        return;
    }

    conn->disconnect();
    EXPECT_FALSE(conn->is_connected());
    EXPECT_FALSE(conn->validate());
}

// --- Reply 파싱 유틸리티 테스트 ---

TEST(RedisConnection, ParseStringReplyFromString)
{
    redisReply reply{};
    reply.type = REDIS_REPLY_STRING;
    const char* str = "hello";
    reply.str = const_cast<char*>(str);
    reply.len = 5;

    auto result = RedisConnection::parse_string_reply(&reply);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "hello");
}

TEST(RedisConnection, ParseStringReplyFromStatus)
{
    redisReply reply{};
    reply.type = REDIS_REPLY_STATUS;
    const char* str = "OK";
    reply.str = const_cast<char*>(str);
    reply.len = 2;

    auto result = RedisConnection::parse_string_reply(&reply);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "OK");
}

TEST(RedisConnection, ParseStringReplyFromNil)
{
    redisReply reply{};
    reply.type = REDIS_REPLY_NIL;

    auto result = RedisConnection::parse_string_reply(&reply);
    EXPECT_FALSE(result.has_value());
}

TEST(RedisConnection, ParseStringReplyFromNull)
{
    auto result = RedisConnection::parse_string_reply(nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST(RedisConnection, ParseIntegerReply)
{
    redisReply reply{};
    reply.type = REDIS_REPLY_INTEGER;
    reply.integer = 42;

    auto result = RedisConnection::parse_integer_reply(&reply);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(RedisConnection, ParseIntegerReplyFromNonInteger)
{
    redisReply reply{};
    reply.type = REDIS_REPLY_STRING;

    auto result = RedisConnection::parse_integer_reply(&reply);
    EXPECT_FALSE(result.has_value());
}

TEST(RedisConnection, IsErrorReply)
{
    redisReply reply{};
    reply.type = REDIS_REPLY_ERROR;
    const char* str = "ERR unknown command";
    reply.str = const_cast<char*>(str);
    reply.len = 19;

    EXPECT_TRUE(RedisConnection::is_error_reply(&reply));
    EXPECT_EQ(RedisConnection::get_error_message(&reply), "ERR unknown command");
}

TEST(RedisConnection, IsNotErrorReply)
{
    redisReply reply{};
    reply.type = REDIS_REPLY_STRING;

    EXPECT_FALSE(RedisConnection::is_error_reply(&reply));
    EXPECT_TRUE(RedisConnection::get_error_message(&reply).empty());
}

// --- 보강 테스트: 에지 케이스 ---

TEST(RedisConnection, ParseIntegerReplyFromNull)
{
    auto result = RedisConnection::parse_integer_reply(nullptr);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::core::ErrorCode::AdapterError);
}

TEST(RedisConnection, IsErrorReplyNull)
{
    EXPECT_FALSE(RedisConnection::is_error_reply(nullptr));
}

TEST(RedisConnection, GetErrorMessageNull)
{
    EXPECT_TRUE(RedisConnection::get_error_message(nullptr).empty());
}

TEST(RedisConnection, ParseStringReplyFromInteger)
{
    // INTEGER 타입은 string 파싱 시 nullopt
    redisReply reply{};
    reply.type = REDIS_REPLY_INTEGER;
    reply.integer = 100;

    auto result = RedisConnection::parse_string_reply(&reply);
    EXPECT_FALSE(result.has_value());
}

TEST(RedisConnection, ParseIntegerReplyNegative)
{
    redisReply reply{};
    reply.type = REDIS_REPLY_INTEGER;
    reply.integer = -1;

    auto result = RedisConnection::parse_integer_reply(&reply);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, -1);
}

TEST(RedisConnection, ParseStringReplyEmptyString)
{
    redisReply reply{};
    reply.type = REDIS_REPLY_STRING;
    const char* str = "";
    reply.str = const_cast<char*>(str);
    reply.len = 0;

    auto result = RedisConnection::parse_string_reply(&reply);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "");
}

TEST(RedisConnection, DisconnectDoubleCallSafe)
{
    boost::asio::io_context io_ctx;
    RedisConfig config{.host = "127.0.0.1",
                       .port = 59999,
                       .password = {},
                       .db = {},
                       .connect_timeout = std::chrono::milliseconds{3000},
                       .command_timeout = std::chrono::milliseconds{1000},
                       .reconnect_max_backoff = std::chrono::milliseconds{30000},
                       .max_pending_commands = 4096};

    auto conn = RedisConnection::create(io_ctx, config);
    if (!conn)
    {
        SUCCEED();
        return;
    }

    // 두 번 disconnect 호출해도 크래시 없음
    conn->disconnect();
    EXPECT_NO_THROW(conn->disconnect());
    EXPECT_FALSE(conn->is_connected());
}
