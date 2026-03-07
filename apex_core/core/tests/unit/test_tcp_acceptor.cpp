#include <apex/core/tcp_acceptor.hpp>
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

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (accept_count.load() < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

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

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (accept_count.load() < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

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

    boost::asio::io_context client_ctx;
    tcp::socket client(client_ctx);
    boost::system::error_code ec;
    client.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port), ec);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(accept_count.load(), 0);
    t.join();
}
