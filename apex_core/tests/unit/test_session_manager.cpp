#include <apex/core/session_manager.hpp>
#include "../test_helpers.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <gtest/gtest.h>

#include <set>

using namespace apex::core;
using apex::test::make_socket_pair;
using boost::asio::ip::tcp;

class SessionManagerTest : public ::testing::Test {
protected:
    void SetUp() override { io_ctx_.restart(); }

    boost::asio::io_context io_ctx_;
};

TEST_F(SessionManagerTest, CreateAndFindSession) {
    SessionManager mgr(0, 300, 512);
    auto [server, client] = make_socket_pair(io_ctx_);

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
    SessionManager mgr(0, 300, 512);
    auto [server, client] = make_socket_pair(io_ctx_);

    auto session = mgr.create_session(std::move(server));
    auto id = session->id();
    mgr.remove_session(id);

    EXPECT_EQ(mgr.session_count(), 0u);
    EXPECT_EQ(mgr.find_session(id), nullptr);

    client.close();
}

TEST_F(SessionManagerTest, HeartbeatTimeout) {
    SessionManager mgr(0, 3, 8);
    auto [server, client] = make_socket_pair(io_ctx_);

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
    EXPECT_EQ(timed_out_session->state(), Session::State::Closed);
    EXPECT_EQ(mgr.session_count(), 0u);

    client.close();
}

TEST_F(SessionManagerTest, TouchResetsTimeout) {
    SessionManager mgr(0, 3, 8);
    auto [server, client] = make_socket_pair(io_ctx_);

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
    SessionManager mgr(0, 300, 512);
    std::vector<tcp::socket> clients;

    for (int i = 0; i < 5; ++i) {
        auto [server, client] = make_socket_pair(io_ctx_);
        [[maybe_unused]] auto s = mgr.create_session(std::move(server));
        clients.push_back(std::move(client));
    }

    EXPECT_EQ(mgr.session_count(), 5u);

    for (auto& c : clients) c.close();
}

TEST_F(SessionManagerTest, DisabledHeartbeat) {
    SessionManager mgr(0, 0, 8);
    auto [server, client] = make_socket_pair(io_ctx_);

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

TEST_F(SessionManagerTest, ForEachVisitsAllSessions) {
    SessionManager mgr(0, 300, 512);
    std::vector<tcp::socket> clients;
    std::set<SessionId> expected_ids;

    for (int i = 0; i < 3; ++i) {
        auto [server, client] = make_socket_pair(io_ctx_);
        auto s = mgr.create_session(std::move(server));
        expected_ids.insert(s->id());
        clients.push_back(std::move(client));
    }

    std::set<SessionId> visited_ids;
    mgr.for_each([&](SessionPtr s) {
        visited_ids.insert(s->id());
    });

    EXPECT_EQ(visited_ids, expected_ids);

    for (auto& c : clients) c.close();
}

TEST_F(SessionManagerTest, FindNonExistentReturnsNull) {
    SessionManager mgr(0, 300, 512);
    EXPECT_EQ(mgr.find_session(9999), nullptr);
}

TEST_F(SessionManagerTest, RemoveNonExistentIsSafe) {
    SessionManager mgr(0, 300, 512);
    mgr.remove_session(9999);  // must not crash
    EXPECT_EQ(mgr.session_count(), 0u);
}

TEST_F(SessionManagerTest, TouchNonExistentIsSafe) {
    SessionManager mgr(0, 300, 512);
    mgr.touch_session(9999);  // must not crash
    EXPECT_EQ(mgr.session_count(), 0u);
}

TEST_F(SessionManagerTest, CreateSessionTransitionsToActive) {
    SessionManager mgr(0, 300, 512);
    auto [server, client] = make_socket_pair(io_ctx_);

    auto session = mgr.create_session(std::move(server));
    ASSERT_NE(session, nullptr);
    // SessionManager::create_session() transitions to Active
    EXPECT_EQ(session->state(), Session::State::Active);
    EXPECT_TRUE(session->is_open());

    client.close();
}
