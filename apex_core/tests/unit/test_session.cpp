#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>
#include "../test_helpers.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/awaitable.hpp>

#include <gtest/gtest.h>

using namespace apex::core;
using apex::test::run_coro;
using apex::test::make_socket_pair;
using boost::asio::ip::tcp;
using boost::asio::awaitable;

class SessionTest : public ::testing::Test {
protected:
    void SetUp() override { io_ctx_.restart(); }

    boost::asio::io_context io_ctx_;
};

TEST_F(SessionTest, InitialState) {
    auto [server, client] = make_socket_pair(io_ctx_);
    Session session(1, std::move(server), 0);

    EXPECT_EQ(session.id(), 1u);
    EXPECT_EQ(session.core_id(), 0u);
    EXPECT_EQ(session.state(), Session::State::Connected);
    EXPECT_TRUE(session.is_open());

    client.close();
}

TEST_F(SessionTest, SendFrame) {
    auto [server, client] = make_socket_pair(io_ctx_);
    auto session = std::make_shared<Session>(1, std::move(server), 0);

    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    WireHeader header{.msg_id = 0x0042,
                      .body_size = static_cast<uint32_t>(payload.size())};
    bool sent = run_coro(io_ctx_, session->async_send(header, payload));
    EXPECT_TRUE(sent);

    std::vector<uint8_t> response(WireHeader::SIZE + payload.size());
    boost::asio::read(client, boost::asio::buffer(response));

    auto parsed = WireHeader::parse(response);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->msg_id, 0x0042);
    EXPECT_EQ(parsed->body_size, 4u);

    client.close();
}

TEST_F(SessionTest, SendAfterClose) {
    auto [server, client] = make_socket_pair(io_ctx_);
    auto session = std::make_shared<Session>(1, std::move(server), 0);

    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);
    EXPECT_FALSE(session->is_open());

    std::vector<uint8_t> payload = {0x01};
    WireHeader header{.msg_id = 1, .body_size = 1};
    bool sent = run_coro(io_ctx_, session->async_send(header, payload));
    EXPECT_FALSE(sent);

    client.close();
}

TEST_F(SessionTest, RecvBufferAccessible) {
    auto [server, client] = make_socket_pair(io_ctx_);
    Session session(1, std::move(server), 0, 4096);

    EXPECT_EQ(session.recv_buffer().capacity(), 4096u);
    EXPECT_EQ(session.recv_buffer().readable_size(), 0u);

    client.close();
}

TEST_F(SessionTest, SendRawSucceeds) {
    auto [server, client] = make_socket_pair(io_ctx_);
    auto session = std::make_shared<Session>(1, std::move(server), 0);

    // Build a raw frame: WireHeader + payload
    std::vector<uint8_t> payload = {0xCA, 0xFE, 0xBA, 0xBE};
    WireHeader header{.msg_id = 0x0077,
                      .body_size = static_cast<uint32_t>(payload.size())};
    auto hdr_bytes = header.serialize();
    std::vector<uint8_t> raw_frame(hdr_bytes.begin(), hdr_bytes.end());
    raw_frame.insert(raw_frame.end(), payload.begin(), payload.end());

    bool sent = run_coro(io_ctx_, session->async_send_raw(raw_frame));
    EXPECT_TRUE(sent);

    // Receive and verify on the client side
    std::vector<uint8_t> received(raw_frame.size());
    boost::asio::read(client, boost::asio::buffer(received));
    EXPECT_EQ(received, raw_frame);

    client.close();
}

TEST_F(SessionTest, SendRawAfterCloseReturnsFalse) {
    auto [server, client] = make_socket_pair(io_ctx_);
    auto session = std::make_shared<Session>(1, std::move(server), 0);

    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);

    std::vector<uint8_t> data = {0x01, 0x02};
    bool sent = run_coro(io_ctx_, session->async_send_raw(data));
    EXPECT_FALSE(sent);

    client.close();
}

TEST_F(SessionTest, DoubleCloseIsSafe) {
    auto [server, client] = make_socket_pair(io_ctx_);
    auto session = std::make_shared<Session>(1, std::move(server), 0);

    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);

    // Second close -- must not crash
    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);

    client.close();
}

TEST_F(SessionTest, SendAfterPeerDisconnect) {
    auto [server_sock, client] = make_socket_pair(io_ctx_);
    auto session = std::make_shared<Session>(1, std::move(server_sock), 0);

    // Close the client side first to simulate peer disconnect
    client.close();

    // Give the OS a moment to propagate the TCP RST/FIN
    io_ctx_.run_for(std::chrono::milliseconds(10));
    io_ctx_.restart();

    // Try to send from the server side -- should handle the error gracefully
    std::vector<uint8_t> payload = {0xDE, 0xAD};
    WireHeader header{.msg_id = 0x0001,
                      .body_size = static_cast<uint32_t>(payload.size())};
    bool sent = run_coro(io_ctx_, session->async_send(header, payload));
    // Send may return true on first attempt (buffered) or false if connection reset.
    // The key assertion is that it does not crash or throw.
    // If first send succeeded, a second send after a brief delay should fail.
    if (sent) {
        io_ctx_.run_for(std::chrono::milliseconds(10));
        io_ctx_.restart();
        bool sent2 = run_coro(io_ctx_, session->async_send(header, payload));
        (void)sent2;  // may or may not fail depending on OS buffering
    }

    // Regardless of send results, close the session and verify terminal state
    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);
    EXPECT_FALSE(session->is_open());
}
