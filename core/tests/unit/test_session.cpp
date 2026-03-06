#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>

#include <gtest/gtest.h>

using namespace apex::core;
using boost::asio::ip::tcp;

class SessionTest : public ::testing::Test {
protected:
    boost::asio::io_context io_ctx_;

    std::pair<tcp::socket, tcp::socket> make_socket_pair() {
        tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
        auto port = acceptor.local_endpoint().port();

        tcp::socket client(io_ctx_);
        client.connect(tcp::endpoint(
            boost::asio::ip::address_v4::loopback(), port));
        auto server = acceptor.accept();
        return {std::move(server), std::move(client)};
    }
};

TEST_F(SessionTest, InitialState) {
    auto [server, client] = make_socket_pair();
    Session session(1, std::move(server), 0);

    EXPECT_EQ(session.id(), 1u);
    EXPECT_EQ(session.core_id(), 0u);
    EXPECT_EQ(session.state(), Session::State::Connected);
    EXPECT_TRUE(session.is_open());

    client.close();
}

TEST_F(SessionTest, SendFrame) {
    auto [server, client] = make_socket_pair();
    auto session = std::make_shared<Session>(1, std::move(server), 0);

    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    WireHeader header{.msg_id = 0x0042,
                      .body_size = static_cast<uint32_t>(payload.size())};
    EXPECT_TRUE(session->send(header, payload));

    std::vector<uint8_t> response(WireHeader::SIZE + payload.size());
    boost::asio::read(client, boost::asio::buffer(response));

    auto parsed = WireHeader::parse(response);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->msg_id, 0x0042);
    EXPECT_EQ(parsed->body_size, 4u);

    client.close();
}

TEST_F(SessionTest, SendAfterClose) {
    auto [server, client] = make_socket_pair();
    auto session = std::make_shared<Session>(1, std::move(server), 0);

    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);
    EXPECT_FALSE(session->is_open());

    std::vector<uint8_t> payload = {0x01};
    WireHeader header{.msg_id = 1, .body_size = 1};
    EXPECT_FALSE(session->send(header, payload));

    client.close();
}

TEST_F(SessionTest, RecvBufferAccessible) {
    auto [server, client] = make_socket_pair();
    Session session(1, std::move(server), 0, 4096);

    EXPECT_EQ(session.recv_buffer().capacity(), 4096u);
    EXPECT_EQ(session.recv_buffer().readable_size(), 0u);

    client.close();
}
