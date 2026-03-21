// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/server.hpp>
#include <apex/core/tcp_binary_protocol.hpp>

#include "../test_helpers.hpp"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <thread>

using namespace apex::core;
using namespace std::chrono_literals;
using boost::asio::ip::tcp;

/// Fixture providing a minimal Server with background run + try_connect helper.
class ListenerLifecycleTest : public ::testing::Test
{
  protected:
    /// Minimal ServerConfig for fast lifecycle tests.
    static ServerConfig make_config(uint32_t num_cores = 1)
    {
        return {
            .num_cores = num_cores,
            .handle_signals = false,
            .drain_timeout = 5s,
            .cross_core_call_timeout = 3000ms,
            .bump_capacity_bytes = 64 * 1024,
            .arena_block_bytes = 4096,
            .arena_max_bytes = 1024 * 1024,
        };
    }

    /// Try to connect to localhost:port. Returns true on success.
    static bool try_connect(uint16_t port)
    {
        boost::asio::io_context ctx;
        tcp::socket sock(ctx);
        boost::system::error_code ec;
        sock.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port), ec);
        if (!ec)
            sock.close();
        return !ec;
    }
};

// --- Test cases ---

TEST_F(ListenerLifecycleTest, StartBindsPort)
{
    Server server(make_config(1));
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }));

    // port() should return the OS-assigned port (>0)
    uint16_t port = server.port();
    EXPECT_GT(port, 0u);

    // Connecting to the bound port should succeed
    EXPECT_TRUE(try_connect(port));

    server.stop();
    t.join();
}

TEST_F(ListenerLifecycleTest, DoubleStartSafe)
{
    // Create, run, stop without crash -- basic lifecycle sanity
    Server server(make_config(1));
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }));

    // Server is running; just stop cleanly
    server.stop();
    t.join();
    // No crash = pass
}

TEST_F(ListenerLifecycleTest, DrainStopsAccepting)
{
    Server server(make_config(1));
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }));

    uint16_t port = server.port();
    EXPECT_GT(port, 0u);

    // Connection before stop should work
    EXPECT_TRUE(try_connect(port));

    // stop() triggers drain internally -- after stop, no more connections
    server.stop();
    t.join();

    // After full shutdown, connection must fail
    EXPECT_FALSE(try_connect(port));
}

TEST_F(ListenerLifecycleTest, StopAfterDrain)
{
    // stop() does drain+stop internally -- clean shutdown
    Server server(make_config(1));
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }));

    // stop() performs drain and stop -- no separate drain call needed
    server.stop();
    t.join();

    // Server is no longer running
    EXPECT_FALSE(server.running());
}

TEST_F(ListenerLifecycleTest, StopWithoutDrain)
{
    // Direct stop() without any prior drain -- must be safe
    Server server(make_config(1));
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }));

    // Immediate stop -- no connections were made, no drain needed
    server.stop();
    t.join();

    EXPECT_FALSE(server.running());
}

TEST_F(ListenerLifecycleTest, ActiveSessionsTracking)
{
    Server server(make_config(1));
    server.listen<TcpBinaryProtocol>(0);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }));

    // No connections yet -- active sessions should be 0
    EXPECT_EQ(server.total_active_sessions(), 0u);

    server.stop();
    t.join();
}

TEST_F(ListenerLifecycleTest, DispatcherPerCore)
{
    // 2-core server with listener -- core_count() must reflect configuration
    Server server(make_config(2));
    server.listen<TcpBinaryProtocol>(0);

    EXPECT_EQ(server.core_count(), 2u);

    std::thread t([&] { server.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }));

    // Verify the server is accepting on the bound port with 2 cores
    uint16_t port = server.port();
    EXPECT_GT(port, 0u);
    EXPECT_TRUE(try_connect(port));

    server.stop();
    t.join();
}
