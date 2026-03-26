// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "../test_helpers.hpp"
#include <apex/core/socket_base.hpp>

#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

using namespace apex::core;

class SocketBaseTest : public ::testing::Test
{
  protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(SocketBaseTest, TcpSocketIsOpenAfterConstruction)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    EXPECT_TRUE(socket->is_open());
    client.close();
}

TEST_F(SocketBaseTest, TcpSocketCloseWorks)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    socket->close();
    EXPECT_FALSE(socket->is_open());
    client.close();
}

TEST_F(SocketBaseTest, TcpSocketCloseIdempotent)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    socket->close();
    socket->close(); // must not crash
    EXPECT_FALSE(socket->is_open());
    client.close();
}

TEST_F(SocketBaseTest, TcpSocketSetNoDelay)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    socket->set_option_no_delay(true); // must not crash
    EXPECT_TRUE(socket->is_open());
    client.close();
}

TEST_F(SocketBaseTest, TcpSocketHandshakeIsNoop)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    auto result = apex::test::run_coro(io_ctx_, socket->async_handshake());
    EXPECT_TRUE(result.has_value());
    client.close();
}

TEST_F(SocketBaseTest, TcpSocketReadWrite)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    const std::vector<uint8_t> send_data = {0x01, 0x02, 0x03, 0x04};

    apex::test::run_coro(io_ctx_, [&]() -> boost::asio::awaitable<void> {
        // Write from client
        co_await boost::asio::async_write(client, boost::asio::buffer(send_data),
                                          boost::asio::as_tuple(boost::asio::use_awaitable));

        // Read through SocketBase
        std::vector<uint8_t> recv_data(4);
        auto [ec, n] = co_await socket->async_read_some(boost::asio::buffer(recv_data));
        EXPECT_FALSE(ec);
        EXPECT_EQ(n, 4u);
        EXPECT_EQ(recv_data, send_data);

        client.close();
        co_return;
    }());
}

TEST_F(SocketBaseTest, TcpSocketAsyncWrite)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    const std::vector<uint8_t> send_data = {0xAA, 0xBB, 0xCC};

    apex::test::run_coro(io_ctx_, [&]() -> boost::asio::awaitable<void> {
        // Write through SocketBase
        auto [ec, n] = co_await socket->async_write(boost::asio::buffer(send_data));
        EXPECT_FALSE(ec);
        EXPECT_EQ(n, 3u);

        // Read from client
        std::vector<uint8_t> recv_data(3);
        auto [ec2, n2] = co_await client.async_read_some(boost::asio::buffer(recv_data),
                                                         boost::asio::as_tuple(boost::asio::use_awaitable));
        EXPECT_FALSE(ec2);
        EXPECT_EQ(n2, 3u);
        EXPECT_EQ(recv_data, send_data);

        client.close();
        co_return;
    }());
}

TEST_F(SocketBaseTest, TcpSocketGetExecutor)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);
    auto socket = make_tcp_socket(std::move(server));

    auto executor = socket->get_executor();
    EXPECT_TRUE(executor != boost::asio::any_io_executor{});
    client.close();
}

TEST_F(SocketBaseTest, PolymorphicAccess)
{
    auto [server, client] = apex::test::make_socket_pair(io_ctx_);

    // unique_ptr<SocketBase> 타입으로 접근
    std::unique_ptr<SocketBase> socket = make_tcp_socket(std::move(server));
    EXPECT_NE(socket, nullptr);
    EXPECT_TRUE(socket->is_open());
    socket->close();
    EXPECT_FALSE(socket->is_open());
    client.close();
}
