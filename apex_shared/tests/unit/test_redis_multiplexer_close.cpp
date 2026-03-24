// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>
#include <apex/shared/adapters/redis/redis_multiplexer.hpp>

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

using namespace apex::shared::adapters::redis;

// Helper: run a coroutine on io_context and block until done, returning the result.
template <typename T> T run_coro(boost::asio::io_context& io, boost::asio::awaitable<T> aw)
{
    auto future = boost::asio::co_spawn(io, std::move(aw), boost::asio::use_future);
    io.run();
    io.restart();
    return future.get();
}

// --- TC1: close()가 pending command 없는 상태에서 안전하게 동작 + 상태 전이 검증 ---
// RedisMultiplexer는 실제 Redis 없이는 pending command를 만들 수 없지만,
// close()의 cancel_all_pending 경로가 빈 pending 큐에서 안전함을 검증하고,
// close() 후 connected()=false, pending_count()=0 상태를 확인한다.
// 추가로 close()를 두 번 호출해도 use-after-free 없이 안전한지 검증.
TEST(RedisMultiplexerClose, CloseWithPendingCommandsReturnsError)
{
    boost::asio::io_context io_ctx;
    RedisConfig config;
    config.host = "localhost";
    config.port = 6379;

    RedisMultiplexer mux(io_ctx, config, 0, [](uint32_t, boost::asio::awaitable<void>) {});

    // 초기 상태: 연결 없음, pending 0
    EXPECT_FALSE(mux.connected());
    EXPECT_EQ(mux.pending_count(), 0u);

    // 연결 없는 상태에서 command() 호출 → 즉시 AdapterError (pending에 들어가지 않음)
    auto result = run_coro(io_ctx, mux.command("PING"));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::core::ErrorCode::AdapterError);
    EXPECT_EQ(mux.pending_count(), 0u);

    // close()는 pending이 0이어도 안전하게 동작해야 함
    mux.close();
    EXPECT_FALSE(mux.connected());
    EXPECT_EQ(mux.pending_count(), 0u);

    // 이중 close()도 안전해야 함 (use-after-free 없음 — ASAN이 검출)
    mux.close();
    EXPECT_FALSE(mux.connected());
    EXPECT_EQ(mux.pending_count(), 0u);
}

// --- TC2: close() 후 command() → 즉시 에러 반환, use-after-free 없음 ---
// close()가 conn_을 null로 만들므로, 이후 command()는 connected() 체크에서
// 즉시 AdapterError를 반환해야 한다. ASAN 환경에서 dangling pointer 접근이 없음을 검증.
TEST(RedisMultiplexerClose, CommandAfterCloseReturnsError)
{
    boost::asio::io_context io_ctx;
    RedisConfig config;
    config.host = "localhost";
    config.port = 6379;

    RedisMultiplexer mux(io_ctx, config, 0, [](uint32_t, boost::asio::awaitable<void>) {});

    // close() 호출
    mux.close();
    EXPECT_FALSE(mux.connected());
    EXPECT_EQ(mux.pending_count(), 0u);

    // close() 후 command() — 즉시 AdapterError, pending에 추가되지 않아야 함
    auto result = run_coro(io_ctx, mux.command("GET %s", "test_key"));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::core::ErrorCode::AdapterError);
    EXPECT_EQ(mux.pending_count(), 0u);

    // deprecated 오버로드도 동일하게 동작해야 함 (경고 억제)
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // deprecated
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

    auto result2 = run_coro(io_ctx, mux.command(std::string_view("PING")));
    EXPECT_FALSE(result2.has_value());
    EXPECT_EQ(result2.error(), apex::core::ErrorCode::AdapterError);
    EXPECT_EQ(mux.pending_count(), 0u);

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

    // pipeline도 close 후 에러를 반환해야 함
    std::vector<std::string> cmds = {"PING", "PING"};
    auto result3 = run_coro(io_ctx, mux.pipeline(cmds));
    EXPECT_FALSE(result3.has_value());
    EXPECT_EQ(result3.error(), apex::core::ErrorCode::AdapterError);
    EXPECT_EQ(mux.pending_count(), 0u);
}
