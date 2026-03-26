// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/server.hpp>
#include <apex/shared/protocols/tcp/tcp_binary_protocol.hpp>

#include "../test_helpers.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace apex::core;
using apex::shared::protocols::tcp::TcpBinaryProtocol;
using namespace std::chrono_literals;

/// Server 에러 경로 단위 테스트.
/// 정상 동작은 test_server_multicore.cpp / test_server_e2e.cpp에서 커버.
/// 이 파일은 에러/엣지 케이스에 집중.

// TC1: 이미 사용 중인 포트에 listen -- system_error 발생
TEST(ServerErrorPaths, ListenOnOccupiedPortFails)
{
    // 첫 번째 서버: 포트 0으로 바인딩 (OS 할당)
    Server server1({.num_cores = 1,
                    .handle_signals = false,
                    .drain_timeout = std::chrono::seconds{25},
                    .cross_core_call_timeout = std::chrono::milliseconds{5000},
                    .bump_capacity_bytes = 64 * 1024,
                    .arena_block_bytes = 4096,
                    .arena_max_bytes = 1024 * 1024,
                    .metrics = {},
                    .admin = {}});
    server1.listen<TcpBinaryProtocol>(0);

    std::thread t1([&] { server1.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server1.running(); }, 5000ms));

    uint16_t occupied_port = server1.port();
    ASSERT_NE(occupied_port, 0u) << "Server1 should have a real port";

    // 두 번째 서버: 동일 포트로 바인딩 시도
    Server server2({.num_cores = 1,
                    .handle_signals = false,
                    .drain_timeout = std::chrono::seconds{25},
                    .cross_core_call_timeout = std::chrono::milliseconds{5000},
                    .bump_capacity_bytes = 64 * 1024,
                    .arena_block_bytes = 4096,
                    .arena_max_bytes = 1024 * 1024,
                    .metrics = {},
                    .admin = {}});
    server2.listen<TcpBinaryProtocol>(occupied_port);

    // listen<P>()는 lazy binding (start()에서 bind) — run()에서 실패
    // Listener::start() 내 TcpAcceptor가 bind 실패 시 예외 발생
    std::exception_ptr eptr;
    std::atomic<bool> t2_done{false};
    std::thread t2([&] {
        try
        {
            server2.run();
        }
        catch (...)
        {
            eptr = std::current_exception();
        }
        t2_done.store(true, std::memory_order_release);
    });

    // 이벤트 대기: server2가 예외 종료하거나 running 상태가 되길 폴링 (BACKLOG-119).
    // 고정 sleep_for(500ms) 대신 wait_for로 TSAN/ASAN 환경에서도 안정.
    apex::test::wait_for([&] { return t2_done.load(std::memory_order_acquire) || server2.running(); },
                         std::chrono::milliseconds(5000));

    // 정리
    if (server2.running())
    {
        server2.stop();
    }
    t2.join();

    server1.stop();
    t1.join();

    // 예외가 발생했거나, server2가 시작되지 않았어야 함
    // (OS에 따라 SO_REUSEADDR 동작이 다를 수 있어 예외 또는 비시작 둘 다 허용)
    if (eptr)
    {
        EXPECT_THROW(std::rethrow_exception(eptr), std::exception);
    }
}

// TC2: run() 이중 호출 -- logic_error
TEST(ServerErrorPaths, DoubleRunThrowsLogicError)
{
    Server server({.num_cores = 1,
                   .handle_signals = false,
                   .drain_timeout = std::chrono::seconds{25},
                   .cross_core_call_timeout = std::chrono::milliseconds{5000},
                   .bump_capacity_bytes = 64 * 1024,
                   .arena_block_bytes = 4096,
                   .arena_max_bytes = 1024 * 1024,
                   .metrics = {},
                   .admin = {}});
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }, 5000ms));

    // 두 번째 run() 호출 — logic_error 예상
    EXPECT_THROW(server.run(), std::logic_error);

    server.stop();
    t.join();
}

// TC3: graceful shutdown -- drain 후 세션 0 확인
TEST(ServerErrorPaths, GracefulShutdownDrainsToZero)
{
    Server server({
        .num_cores = 1,
        .heartbeat_timeout_ticks = 0,
        .handle_signals = false,
        .metrics = {},
        .admin = {},
    });
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }, 5000ms));

    // 클라이언트 연결
    boost::asio::io_context client_ctx;
    boost::asio::ip::tcp::socket client(client_ctx);
    client.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), server.port()));

    // 세션 활성화 대기
    ASSERT_TRUE(apex::test::wait_for([&] { return server.total_active_sessions() >= 1u; }, 3000ms));

    // stop 호출 — graceful shutdown
    auto before = std::chrono::steady_clock::now();
    server.stop();
    t.join();
    auto elapsed = std::chrono::steady_clock::now() - before;

    // drain_timeout(25s) 내에 종료되었어야 함
    EXPECT_LT(elapsed, 10s);
    EXPECT_FALSE(server.running());
    EXPECT_EQ(server.total_active_sessions(), 0u);

    client.close();
}

// TC4: stop() 재진입 안전성
TEST(ServerErrorPaths, DoubleStopIsSafe)
{
    Server server({.num_cores = 1,
                   .handle_signals = false,
                   .drain_timeout = std::chrono::seconds{25},
                   .cross_core_call_timeout = std::chrono::milliseconds{5000},
                   .bump_capacity_bytes = 64 * 1024,
                   .arena_block_bytes = 4096,
                   .arena_max_bytes = 1024 * 1024,
                   .metrics = {},
                   .admin = {}});
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }, 5000ms));

    // 두 번 stop() 호출 — 크래시 없어야 함
    server.stop();
    server.stop();

    t.join();
    EXPECT_FALSE(server.running());
}

// TC5: listen 없이 run -- 서비스만 있는 서버
TEST(ServerErrorPaths, RunWithoutListenersWorks)
{
    Server server({.num_cores = 1,
                   .handle_signals = false,
                   .drain_timeout = std::chrono::seconds{25},
                   .cross_core_call_timeout = std::chrono::milliseconds{5000},
                   .bump_capacity_bytes = 64 * 1024,
                   .arena_block_bytes = 4096,
                   .arena_max_bytes = 1024 * 1024,
                   .metrics = {},
                   .admin = {}});
    // listen<P>() 호출 없음 — listeners_ 비어있음

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }, 5000ms));

    // 포트 0 (리스너 없음)
    EXPECT_EQ(server.port(), 0u);
    EXPECT_EQ(server.total_active_sessions(), 0u);

    server.stop();
    t.join();
}
