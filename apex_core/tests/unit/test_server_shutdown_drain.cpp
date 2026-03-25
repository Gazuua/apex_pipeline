// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

/// Server shutdown drain timeout 테스트.
/// test_server_error_paths.cpp의 GracefulShutdownDrainsToZero와 유사하나,
/// drain_timeout을 극히 짧게 설정하여 타임아웃 경로를 검증한다.

#include <apex/core/server.hpp>
#include <apex/shared/protocols/tcp/tcp_binary_protocol.hpp>

#include "../test_helpers.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace apex::core;
using apex::shared::protocols::tcp::TcpBinaryProtocol;
using namespace std::chrono_literals;

// TC1: drain_timeout이 매우 짧을 때 (100ms) 연결이 남아있어도 강제 종료되는 경로
// poll_shutdown()에서 drain_timeout 만료 시 finalize_shutdown()으로 직행하는 분기를 검증.
TEST(ServerShutdownDrain, ShortDrainTimeoutForcesShutdown)
{
    // drain_timeout = 100ms (극히 짧음)
    Server server({
        .num_cores = 1,
        .heartbeat_timeout_ticks = 0,
        .handle_signals = false,
        .drain_timeout = std::chrono::seconds{0}, // 0초 — 즉시 타임아웃
        .metrics = {},
    });
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }, 5000ms));

    // 클라이언트 연결 — shutdown 시 drain 대상
    boost::asio::io_context client_ctx;
    boost::asio::ip::tcp::socket client(client_ctx);
    client.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), server.port()));

    // 세션 활성화 대기
    ASSERT_TRUE(apex::test::wait_for([&] { return server.total_active_sessions() >= 1u; }, 3000ms));

    // stop 호출 — drain_timeout=0s이므로 즉시 강제 종료 경로 진입
    auto before = std::chrono::steady_clock::now();
    server.stop();
    t.join();
    auto elapsed = std::chrono::steady_clock::now() - before;

    // drain_timeout이 0s이므로 빠르게 종료되어야 함 (5초 이내)
    EXPECT_LT(elapsed, 5s) << "Server should shut down quickly with 0s drain timeout";
    EXPECT_FALSE(server.running());

    client.close();
}

// TC2: drain_timeout 내에 클라이언트가 끊기면 정상 drain
// drain_timeout이 짧지만 클라이언트가 먼저 끊기면 정상 경로를 타는지 확인.
TEST(ServerShutdownDrain, ClientDisconnectBeforeDrainTimeout)
{
    Server server({
        .num_cores = 1,
        .heartbeat_timeout_ticks = 0,
        .handle_signals = false,
        .drain_timeout = std::chrono::seconds{10},
        .metrics = {},
    });
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }, 5000ms));

    // 클라이언트 연결
    boost::asio::io_context client_ctx;
    boost::asio::ip::tcp::socket client(client_ctx);
    client.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), server.port()));

    ASSERT_TRUE(apex::test::wait_for([&] { return server.total_active_sessions() >= 1u; }, 3000ms));

    // 클라이언트를 먼저 끊은 뒤 stop — 세션이 이미 정리되어 drain 즉시 완료
    client.close();
    ASSERT_TRUE(apex::test::wait_for([&] { return server.total_active_sessions() == 0u; }, 3000ms));

    auto before = std::chrono::steady_clock::now();
    server.stop();
    t.join();
    auto elapsed = std::chrono::steady_clock::now() - before;

    // 이미 drain된 상태이므로 빠르게 종료
    EXPECT_LT(elapsed, 5s);
    EXPECT_FALSE(server.running());
    EXPECT_EQ(server.total_active_sessions(), 0u);
}

// TC3: 다중 클라이언트 연결 상태에서 drain timeout
// 여러 세션이 남아있어도 타임아웃 후 강제 종료되는지 검증.
TEST(ServerShutdownDrain, MultipleClientsForceShutdownOnTimeout)
{
    Server server({
        .num_cores = 1,
        .heartbeat_timeout_ticks = 0,
        .handle_signals = false,
        .drain_timeout = std::chrono::seconds{0}, // 즉시 타임아웃
        .metrics = {},
    });
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }, 5000ms));

    // 다중 클라이언트 연결
    constexpr int NUM_CLIENTS = 5;
    boost::asio::io_context client_ctx;
    std::vector<boost::asio::ip::tcp::socket> clients;
    clients.reserve(NUM_CLIENTS);

    for (int i = 0; i < NUM_CLIENTS; ++i)
    {
        clients.emplace_back(client_ctx);
        clients.back().connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), server.port()));
    }

    ASSERT_TRUE(apex::test::wait_for(
        [&] { return server.total_active_sessions() >= static_cast<uint32_t>(NUM_CLIENTS); }, 5000ms));

    // stop — drain_timeout=0s이므로 즉시 강제 종료
    auto before = std::chrono::steady_clock::now();
    server.stop();
    t.join();
    auto elapsed = std::chrono::steady_clock::now() - before;

    EXPECT_LT(elapsed, 5s);
    EXPECT_FALSE(server.running());

    for (auto& c : clients)
        c.close();
}
