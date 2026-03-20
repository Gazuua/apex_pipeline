// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/adapters/redis/redis_multiplexer.hpp>

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <hiredis/hiredis.h>

using namespace apex::shared::adapters::redis;

class RedisRoundtripTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        config_.host = "localhost";
        config_.port = 6379;
    }
    RedisConfig config_;
    boost::asio::io_context io_ctx_;
};

TEST_F(RedisRoundtripTest, SetAndGet)
{
    bool test_passed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            RedisMultiplexer mux(io_ctx_, config_);

            if (!mux.connected())
            {
                GTEST_SKIP() << "Redis not available on localhost:6379";
                co_return;
            }

            // SET (parameterized binding)
            auto set_result = co_await mux.command("SET %s %s", "test:integration:key", "hello-redis");
            EXPECT_TRUE(set_result.has_value()) << "SET failed - is Redis running on localhost:6379?";
            if (!set_result.has_value())
                co_return;
            EXPECT_EQ(set_result->type, REDIS_REPLY_STATUS);

            // GET (parameterized binding)
            auto get_result = co_await mux.command("GET %s", "test:integration:key");
            EXPECT_TRUE(get_result.has_value());
            if (get_result.has_value())
            {
                EXPECT_EQ(get_result->type, REDIS_REPLY_STRING);
                EXPECT_EQ(get_result->str, "hello-redis");
            }

            // DEL (parameterized binding)
            auto del_result = co_await mux.command("DEL %s", "test:integration:key");
            EXPECT_TRUE(del_result.has_value());
            if (del_result.has_value())
            {
                EXPECT_EQ(del_result->type, REDIS_REPLY_INTEGER);
                EXPECT_EQ(del_result->integer, 1);
            }

            // GET after DEL (parameterized binding)
            auto get2_result = co_await mux.command("GET %s", "test:integration:key");
            EXPECT_TRUE(get2_result.has_value());
            if (get2_result.has_value())
            {
                EXPECT_EQ(get2_result->type, REDIS_REPLY_NIL);
            }

            co_await mux.close();
            test_passed = true;
            co_return;
        },
        boost::asio::detached);
    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}

TEST_F(RedisRoundtripTest, PipelineCommands)
{
    bool test_passed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> boost::asio::awaitable<void> {
            RedisMultiplexer mux(io_ctx_, config_);

            if (!mux.connected())
            {
                GTEST_SKIP() << "Redis not available on localhost:6379";
                co_return;
            }

            // Pipeline: SET then GET then DEL
            std::vector<std::string> cmds = {"SET test:integration:pipeline ok", "GET test:integration:pipeline",
                                             "DEL test:integration:pipeline"};
            auto results = co_await mux.pipeline(cmds);
            EXPECT_TRUE(results.has_value());
            if (results.has_value())
            {
                EXPECT_EQ(results->size(), 3u);
                EXPECT_EQ((*results)[0].type, REDIS_REPLY_STATUS);
                EXPECT_EQ((*results)[1].type, REDIS_REPLY_STRING);
                EXPECT_EQ((*results)[1].str, "ok");
                EXPECT_EQ((*results)[2].type, REDIS_REPLY_INTEGER);
                EXPECT_EQ((*results)[2].integer, 1);
            }

            co_await mux.close();
            test_passed = true;
            co_return;
        },
        boost::asio::detached);
    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}
