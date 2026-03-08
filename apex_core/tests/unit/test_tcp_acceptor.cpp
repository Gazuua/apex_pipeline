#include <apex/core/tcp_acceptor.hpp>
#include "../test_helpers.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <thread>

using namespace apex::core;
using boost::asio::ip::tcp;

TEST(TcpAcceptor, AcceptConnection) {
    boost::asio::io_context io_ctx;
    std::atomic<int> accept_count{0};

    TcpAcceptor acceptor(io_ctx, 0, [&](tcp::socket) { ++accept_count; });
    acceptor.start();
    EXPECT_GT(acceptor.port(), 0);

    std::thread t([&] { io_ctx.run(); });

    boost::asio::io_context client_ctx;
    tcp::socket client(client_ctx);
    client.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), acceptor.port()));

    ASSERT_TRUE(apex::test::wait_for([&] { return accept_count.load() >= 1; },
                                      std::chrono::milliseconds(2000)));
    EXPECT_EQ(accept_count.load(), 1);
    client.close();
    acceptor.stop();
    t.join();
}

TEST(TcpAcceptor, MultipleConnections) {
    boost::asio::io_context io_ctx;
    std::atomic<int> accept_count{0};

    TcpAcceptor acceptor(io_ctx, 0, [&](tcp::socket) { ++accept_count; });
    acceptor.start();

    std::thread t([&] { io_ctx.run(); });

    boost::asio::io_context client_ctx;
    std::vector<tcp::socket> clients;
    for (int i = 0; i < 3; ++i) {
        clients.emplace_back(client_ctx);
        clients.back().connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), acceptor.port()));
    }

    ASSERT_TRUE(apex::test::wait_for([&] { return accept_count.load() >= 3; },
                                      std::chrono::milliseconds(2000)));
    EXPECT_EQ(accept_count.load(), 3);
    for (auto& c : clients) c.close();
    acceptor.stop();
    t.join();
}

TEST(TcpAcceptor, DoubleStartIsSafe) {
    boost::asio::io_context io_ctx;
    std::atomic<int> accept_count{0};

    TcpAcceptor acceptor(io_ctx, 0, [&](tcp::socket) { ++accept_count; });
    acceptor.start();
    EXPECT_TRUE(acceptor.running());

    // Second start -- must not crash
    acceptor.start();
    EXPECT_TRUE(acceptor.running());

    acceptor.stop();
    io_ctx.run();
}

TEST(TcpAcceptor, StopPreventsNewAccepts) {
    boost::asio::io_context io_ctx;
    std::atomic<int> accept_count{0};

    TcpAcceptor acceptor(io_ctx, 0, [&](tcp::socket) { ++accept_count; });
    acceptor.start();
    auto port = acceptor.port();

    std::thread t([&] { io_ctx.run(); });

    acceptor.stop();
    EXPECT_FALSE(acceptor.running());

    // After stop, attempt connection — may succeed at TCP level (OS backlog)
    // but the acceptor must not invoke the callback.
    boost::asio::io_context client_ctx;
    tcp::socket client(client_ctx);
    boost::system::error_code ec;
    client.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port), ec);

    // Run io_ctx to completion — any pending handlers will execute.
    // The stopped acceptor should not post any more accept callbacks.
    client.close();
    t.join();

    // Deterministic: io_ctx has drained all handlers; accept_count must be 0.
    EXPECT_EQ(accept_count.load(), 0);
}

TEST(TcpAcceptor, IPv6AcceptConnection) {
    // Skip if IPv6 is not available on this machine
    boost::asio::io_context probe_ctx;
    {
        boost::system::error_code ec;
        tcp::acceptor probe(probe_ctx);
        probe.open(tcp::v6(), ec);
        if (ec) {
            GTEST_SKIP() << "IPv6 not available: " << ec.message();
        }
        probe.bind(tcp::endpoint(tcp::v6(), 0), ec);
        if (ec) {
            GTEST_SKIP() << "IPv6 bind failed: " << ec.message();
        }
        probe.close();
    }

    boost::asio::io_context io_ctx;
    std::atomic<int> accept_count{0};

    TcpAcceptor acceptor(io_ctx, 0, [&](tcp::socket) { ++accept_count; }, tcp::v6());
    acceptor.start();
    EXPECT_GT(acceptor.port(), 0);

    std::thread t([&] { io_ctx.run(); });

    boost::asio::io_context client_ctx;
    tcp::socket client(client_ctx);
    boost::system::error_code ec;
    client.connect(tcp::endpoint(boost::asio::ip::address_v6::loopback(), acceptor.port()), ec);

    if (ec) {
        // IPv6 connection failed (e.g., loopback disabled) — not a test failure
        acceptor.stop();
        t.join();
        GTEST_SKIP() << "IPv6 loopback connection failed: " << ec.message();
    }

    ASSERT_TRUE(apex::test::wait_for([&] { return accept_count.load() >= 1; },
                                      std::chrono::milliseconds(2000)));
    EXPECT_EQ(accept_count.load(), 1);
    client.close();
    acceptor.stop();
    t.join();
}
