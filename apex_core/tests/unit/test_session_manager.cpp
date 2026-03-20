// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "../test_helpers.hpp"
#include <apex/core/session_manager.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <gtest/gtest.h>

#include <set>

using namespace apex::core;
using apex::test::make_socket_pair;
using boost::asio::ip::tcp;

class SessionManagerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        io_ctx_.restart();
    }

    boost::asio::io_context io_ctx_;
};

TEST_F(SessionManagerTest, CreateAndFindSession)
{
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

TEST_F(SessionManagerTest, RemoveSession)
{
    SessionManager mgr(0, 300, 512);
    auto [server, client] = make_socket_pair(io_ctx_);

    auto session = mgr.create_session(std::move(server));
    auto id = session->id();
    mgr.remove_session(id);

    EXPECT_EQ(mgr.session_count(), 0u);
    EXPECT_EQ(mgr.find_session(id), nullptr);

    client.close();
}

TEST_F(SessionManagerTest, HeartbeatTimeout)
{
    SessionManager mgr(0, 3, 8);
    auto [server, client] = make_socket_pair(io_ctx_);

    SessionPtr timed_out_session;
    mgr.set_timeout_callback([&](SessionPtr s) { timed_out_session = s; });

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

TEST_F(SessionManagerTest, TouchResetsTimeout)
{
    SessionManager mgr(0, 3, 8);
    auto [server, client] = make_socket_pair(io_ctx_);

    SessionPtr timed_out_session;
    mgr.set_timeout_callback([&](SessionPtr s) { timed_out_session = s; });

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

TEST_F(SessionManagerTest, MultipleSessions)
{
    SessionManager mgr(0, 300, 512);
    std::vector<tcp::socket> clients;

    for (int i = 0; i < 5; ++i)
    {
        auto [server, client] = make_socket_pair(io_ctx_);
        [[maybe_unused]] auto s = mgr.create_session(std::move(server));
        clients.push_back(std::move(client));
    }

    EXPECT_EQ(mgr.session_count(), 5u);

    for (auto& c : clients)
        c.close();
}

TEST_F(SessionManagerTest, DisabledHeartbeat)
{
    SessionManager mgr(0, 0, 8);
    auto [server, client] = make_socket_pair(io_ctx_);

    SessionPtr timed_out_session;
    mgr.set_timeout_callback([&](SessionPtr s) { timed_out_session = s; });

    [[maybe_unused]] auto session = mgr.create_session(std::move(server));

    for (int i = 0; i < 100; ++i)
        mgr.tick();

    EXPECT_EQ(timed_out_session, nullptr);
    EXPECT_EQ(mgr.session_count(), 1u);

    client.close();
}

TEST_F(SessionManagerTest, ForEachVisitsAllSessions)
{
    SessionManager mgr(0, 300, 512);
    std::vector<tcp::socket> clients;
    std::set<SessionId> expected_ids;

    for (int i = 0; i < 3; ++i)
    {
        auto [server, client] = make_socket_pair(io_ctx_);
        auto s = mgr.create_session(std::move(server));
        expected_ids.insert(s->id());
        clients.push_back(std::move(client));
    }

    std::set<SessionId> visited_ids;
    mgr.for_each([&](SessionPtr s) { visited_ids.insert(s->id()); });

    EXPECT_EQ(visited_ids, expected_ids);

    for (auto& c : clients)
        c.close();
}

TEST_F(SessionManagerTest, FindNonExistentReturnsNull)
{
    SessionManager mgr(0, 300, 512);
    EXPECT_EQ(mgr.find_session(make_session_id(9999)), nullptr);
}

TEST_F(SessionManagerTest, RemoveNonExistentIsSafe)
{
    SessionManager mgr(0, 300, 512);
    mgr.remove_session(make_session_id(9999)); // must not crash
    EXPECT_EQ(mgr.session_count(), 0u);
}

TEST_F(SessionManagerTest, TouchNonExistentIsSafe)
{
    SessionManager mgr(0, 300, 512);
    mgr.touch_session(make_session_id(9999)); // must not crash
    EXPECT_EQ(mgr.session_count(), 0u);
}

TEST_F(SessionManagerTest, CreateSessionTransitionsToActive)
{
    SessionManager mgr(0, 300, 512);
    auto [server, client] = make_socket_pair(io_ctx_);

    auto session = mgr.create_session(std::move(server));
    ASSERT_NE(session, nullptr);
    // SessionManager::create_session() transitions to Active
    EXPECT_EQ(session->state(), Session::State::Active);
    EXPECT_TRUE(session->is_open());

    client.close();
}

TEST_F(SessionManagerTest, SlabAllocatorAllocation)
{
    // max_sessions_per_core = 2 → 3rd session falls back to heap
    SessionManager mgr(0, 0, 8, 8192, 2);
    std::vector<tcp::socket> clients;
    std::vector<SessionPtr> sessions;

    for (int i = 0; i < 3; ++i)
    {
        auto [server, client] = make_socket_pair(io_ctx_);
        auto s = mgr.create_session(std::move(server));
        ASSERT_NE(s, nullptr);
        sessions.push_back(s);
        clients.push_back(std::move(client));
    }

    EXPECT_EQ(mgr.session_count(), 3u);

    // All sessions should be functional regardless of allocation source
    for (auto& s : sessions)
    {
        EXPECT_TRUE(s->is_open());
    }

    for (auto& c : clients)
        c.close();
}

TEST_F(SessionManagerTest, SlabAllocatorReclaimAfterRemove)
{
    SessionManager mgr(0, 0, 8, 8192, 2);
    std::vector<tcp::socket> clients;

    // Fill pool (2 sessions)
    auto [s1, c1] = make_socket_pair(io_ctx_);
    auto session1 = mgr.create_session(std::move(s1));
    auto id1 = session1->id();
    clients.push_back(std::move(c1));

    auto [s2, c2] = make_socket_pair(io_ctx_);
    auto session2 = mgr.create_session(std::move(s2));
    clients.push_back(std::move(c2));

    // Remove session1 → slot returns to pool
    session1.reset();
    mgr.remove_session(id1);

    // Create another — should use reclaimed pool slot, not heap
    auto [s3, c3] = make_socket_pair(io_ctx_);
    auto session3 = mgr.create_session(std::move(s3));
    ASSERT_NE(session3, nullptr);
    EXPECT_TRUE(session3->is_open());
    clients.push_back(std::move(c3));

    for (auto& c : clients)
        c.close();
}

TEST_F(SessionManagerTest, SlabAllocatorReturnOnRefcountZero)
{
    SessionManager mgr(0, 0, 8, 8192, 4);
    auto [server, client] = make_socket_pair(io_ctx_);

    auto session = mgr.create_session(std::move(server));
    auto id = session->id();

    // remove from mgr → only session local ref remains
    mgr.remove_session(id);
    EXPECT_EQ(mgr.session_count(), 0u);

    // reset → refcount 0 → pool_owner_->destroy()
    session.reset();

    // If pool return succeeded, new allocation should reuse the slot
    auto [s2, c2] = make_socket_pair(io_ctx_);
    auto session2 = mgr.create_session(std::move(s2));
    EXPECT_NE(session2, nullptr);

    client.close();
    c2.close();
}

TEST_F(SessionManagerTest, SlabAllocatorMixedAllocationRemoval)
{
    SessionManager mgr(0, 0, 8, 8192, 2);
    std::vector<tcp::socket> clients;
    std::vector<SessionPtr> sessions;
    std::vector<SessionId> ids;

    for (int i = 0; i < 3; ++i)
    {
        auto [server, client] = make_socket_pair(io_ctx_);
        auto s = mgr.create_session(std::move(server));
        ids.push_back(s->id());
        sessions.push_back(s);
        clients.push_back(std::move(client));
    }

    // Remove in reverse: heap(3rd) first, then pool(2nd, 1st)
    for (int i = 2; i >= 0; --i)
    {
        sessions[static_cast<size_t>(i)].reset();
        mgr.remove_session(ids[static_cast<size_t>(i)]);
    }
    EXPECT_EQ(mgr.session_count(), 0u);

    for (auto& c : clients)
        c.close();
}
