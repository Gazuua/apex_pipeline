#include <apex/core/session_manager.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <gtest/gtest.h>

using namespace apex::core;
using boost::asio::ip::tcp;

class SessionManagerTest : public ::testing::Test {
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

TEST_F(SessionManagerTest, CreateAndFindSession) {
    SessionManager mgr(0, 300, 64);
    auto [server, client] = make_socket_pair();

    auto session = mgr.create_session(std::move(server));
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->core_id(), 0u);
    EXPECT_TRUE(session->is_open());
    EXPECT_EQ(mgr.session_count(), 1u);

    auto found = mgr.find_session(session->id());
    EXPECT_EQ(found, session);

    client.close();
}

TEST_F(SessionManagerTest, RemoveSession) {
    SessionManager mgr(0, 300, 64);
    auto [server, client] = make_socket_pair();

    auto session = mgr.create_session(std::move(server));
    auto id = session->id();
    mgr.remove_session(id);

    EXPECT_EQ(mgr.session_count(), 0u);
    EXPECT_EQ(mgr.find_session(id), nullptr);

    client.close();
}

TEST_F(SessionManagerTest, HeartbeatTimeout) {
    SessionManager mgr(0, 3, 8);
    auto [server, client] = make_socket_pair();

    SessionPtr timed_out_session;
    mgr.set_timeout_callback([&](SessionPtr s) {
        timed_out_session = s;
    });

    auto session = mgr.create_session(std::move(server));
    auto id = session->id();

    mgr.tick();
    mgr.tick();
    mgr.tick();

    // TimingWheel fires on tick AFTER deadline. schedule(3) -> deadline=current+3.
    // Need one more tick to fire.
    mgr.tick();

    EXPECT_NE(timed_out_session, nullptr);
    EXPECT_EQ(timed_out_session->id(), id);
    EXPECT_EQ(mgr.session_count(), 0u);

    client.close();
}

TEST_F(SessionManagerTest, TouchResetsTimeout) {
    SessionManager mgr(0, 3, 8);
    auto [server, client] = make_socket_pair();

    SessionPtr timed_out_session;
    mgr.set_timeout_callback([&](SessionPtr s) {
        timed_out_session = s;
    });

    auto session = mgr.create_session(std::move(server));

    mgr.tick();
    mgr.tick();
    mgr.touch_session(session->id());
    mgr.tick();
    mgr.tick();

    // touch reset the timer, so not timed out yet
    EXPECT_EQ(timed_out_session, nullptr);
    EXPECT_EQ(mgr.session_count(), 1u);

    // Need more ticks to reach new deadline
    mgr.tick();
    mgr.tick();

    EXPECT_NE(timed_out_session, nullptr);

    client.close();
}

TEST_F(SessionManagerTest, MultipleSessions) {
    SessionManager mgr(0, 300, 64);
    std::vector<tcp::socket> clients;

    for (int i = 0; i < 5; ++i) {
        auto [server, client] = make_socket_pair();
        [[maybe_unused]] auto s = mgr.create_session(std::move(server));
        clients.push_back(std::move(client));
    }

    EXPECT_EQ(mgr.session_count(), 5u);

    for (auto& c : clients) c.close();
}

TEST_F(SessionManagerTest, DisabledHeartbeat) {
    SessionManager mgr(0, 0, 8);
    auto [server, client] = make_socket_pair();

    SessionPtr timed_out_session;
    mgr.set_timeout_callback([&](SessionPtr s) {
        timed_out_session = s;
    });

    [[maybe_unused]] auto session = mgr.create_session(std::move(server));

    for (int i = 0; i < 100; ++i) mgr.tick();

    EXPECT_EQ(timed_out_session, nullptr);
    EXPECT_EQ(mgr.session_count(), 1u);

    client.close();
}
