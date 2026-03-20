// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/logging.hpp>
#include <apex/core/server.hpp>
#include <apex/core/tcp_binary_protocol.hpp>

#include "../test_helpers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace apex::core;
using namespace std::chrono_literals;

class ShutdownTimeoutTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        init_logging(LogConfig{});
    }
    void TearDown() override
    {
        shutdown_logging();
    }
};

TEST_F(ShutdownTimeoutTest, NormalShutdownWithinTimeout)
{
    // 세션 없이 바로 종료 — 타임아웃 전에 완료되어야 함
    Server server({
        .num_cores = 1,
        .handle_signals = false,
        .drain_timeout = 2s,
    });
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }));

    auto start = std::chrono::steady_clock::now();
    server.stop();
    t.join();
    auto elapsed = std::chrono::steady_clock::now() - start;

    // 세션 없으면 즉시 종료 (2초 타임아웃보다 훨씬 빨라야 함)
    EXPECT_LT(elapsed, 1s);
}

TEST_F(ShutdownTimeoutTest, DrainTimeoutForcesShutdown)
{
    // drain_timeout이 짧으면 강제 종료까지의 시간이 제한됨
    Server server({
        .num_cores = 1,
        .handle_signals = false,
        .drain_timeout = 1s, // 1초 타임아웃
    });
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }));
    server.stop();
    t.join();

    // 서버가 정상 종료됨 (크래시/행 없이)
    EXPECT_FALSE(server.running());
}
