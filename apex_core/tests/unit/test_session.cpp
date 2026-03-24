// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "../test_helpers.hpp"
#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

using namespace apex::core;
using apex::test::make_socket_pair;
using apex::test::run_coro;
using boost::asio::awaitable;
using boost::asio::ip::tcp;

class SessionTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        io_ctx_.restart();
    }

    boost::asio::io_context io_ctx_;
};

TEST_F(SessionTest, InitialState)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    Session session(make_session_id(1), std::move(server), 0);

    EXPECT_EQ(session.id(), make_session_id(1));
    EXPECT_EQ(session.core_id(), 0u);
    EXPECT_EQ(session.state(), Session::State::Connected);
    EXPECT_TRUE(session.is_open());

    client.close();
}

TEST_F(SessionTest, SendFrame)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    WireHeader header{.msg_id = 0x0042, .body_size = static_cast<uint32_t>(payload.size()), .reserved = {}};
    auto result = run_coro(io_ctx_, session->async_send(header, payload));
    EXPECT_TRUE(result.has_value());

    std::vector<uint8_t> response(WireHeader::SIZE + payload.size());
    boost::asio::read(client, boost::asio::buffer(response));

    auto parsed = WireHeader::parse(response);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->msg_id, 0x0042);
    EXPECT_EQ(parsed->body_size, 4u);

    client.close();
}

TEST_F(SessionTest, SendAfterClose)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);
    EXPECT_FALSE(session->is_open());

    std::vector<uint8_t> payload = {0x01};
    WireHeader header{.msg_id = 1, .body_size = 1, .reserved = {}};
    auto result = run_coro(io_ctx_, session->async_send(header, payload));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::SessionClosed);

    client.close();
}

TEST_F(SessionTest, RecvBufferDefaultCapacity)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    Session session(make_session_id(1), std::move(server), 0);

    // Default recv_buf_capacity is 8192 (see session.hpp constructor default)
    EXPECT_EQ(session.recv_buffer().capacity(), 8192u);
    EXPECT_EQ(session.recv_buffer().readable_size(), 0u);

    client.close();
}

TEST_F(SessionTest, RecvBufferAccessible)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    Session session(make_session_id(1), std::move(server), 0, 4096);

    EXPECT_EQ(session.recv_buffer().capacity(), 4096u);
    EXPECT_EQ(session.recv_buffer().readable_size(), 0u);

    client.close();
}

TEST_F(SessionTest, SendRawSucceeds)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

    // Build a raw frame: WireHeader + payload
    std::vector<uint8_t> payload = {0xCA, 0xFE, 0xBA, 0xBE};
    WireHeader header{.msg_id = 0x0077, .body_size = static_cast<uint32_t>(payload.size()), .reserved = {}};
    auto hdr_bytes = header.serialize();
    std::vector<uint8_t> raw_frame(hdr_bytes.begin(), hdr_bytes.end());
    raw_frame.insert(raw_frame.end(), payload.begin(), payload.end());

    auto result = run_coro(io_ctx_, session->async_send_raw(raw_frame));
    EXPECT_TRUE(result.has_value());

    // Receive and verify on the client side
    std::vector<uint8_t> received(raw_frame.size());
    boost::asio::read(client, boost::asio::buffer(received));
    EXPECT_EQ(received, raw_frame);

    client.close();
}

TEST_F(SessionTest, SendRawAfterCloseReturnsFalse)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);

    std::vector<uint8_t> data = {0x01, 0x02};
    auto result = run_coro(io_ctx_, session->async_send_raw(data));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::SessionClosed);

    client.close();
}

TEST_F(SessionTest, DoubleCloseIsSafe)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);

    // Second close -- must not crash
    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);

    client.close();
}

// Send after peer disconnect is inherently non-deterministic:
// - The OS may buffer the first send (returning success) or immediately report
//   a connection reset (returning failure), depending on TCP stack timing.
// - The key invariant is that the Session handles it gracefully without crash.
TEST_F(SessionTest, SendAfterPeerDisconnect_DoesNotCrash)
{
    auto [server_sock, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), std::move(server_sock), 0));

    // Close the client side first to simulate peer disconnect
    client.close();

    // Give the OS a moment to propagate the TCP RST/FIN
    io_ctx_.run_for(std::chrono::milliseconds(10));
    io_ctx_.restart();

    // Try to send from the server side -- should handle the error gracefully
    std::vector<uint8_t> payload = {0xDE, 0xAD};
    WireHeader header{.msg_id = 0x0001, .body_size = static_cast<uint32_t>(payload.size()), .reserved = {}};
    auto result = run_coro(io_ctx_, session->async_send(header, payload));
    // Send may succeed (buffered) or fail (connection reset).
    if (result.has_value())
    {
        io_ctx_.run_for(std::chrono::milliseconds(10));
        io_ctx_.restart();
        auto result2 = run_coro(io_ctx_, session->async_send(header, payload));
        (void)result2;
    }

    // Regardless of send results, close the session and verify terminal state
    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);
    EXPECT_FALSE(session->is_open());
}

TEST_F(SessionTest, IntrusiveRefcount)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    auto* raw = new Session(make_session_id(1), std::move(server), 0, 8192);

    EXPECT_EQ(raw->refcount(), 0u);

    {
        SessionPtr p1(raw);
        EXPECT_EQ(raw->refcount(), 1u);

        SessionPtr p2 = p1;
        EXPECT_EQ(raw->refcount(), 2u);
    }
    // p1, p2 destroyed → refcount 0 → delete (ASAN leak check)

    client.close();
}

TEST_F(SessionTest, IntrusiveMoveSemantics)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    auto* raw = new Session(make_session_id(1), std::move(server), 0, 8192);

    SessionPtr p1(raw);
    EXPECT_EQ(raw->refcount(), 1u);

    SessionPtr p2 = std::move(p1);
    EXPECT_EQ(p1.get(), nullptr);
    EXPECT_EQ(raw->refcount(), 1u);

    client.close();
}

// --- Write Queue Tests (v0.5 C-prep) ---

TEST_F(SessionTest, EnqueueWriteAndReceive)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    auto result = session->enqueue_write(data);
    ASSERT_TRUE(result.has_value());

    // Run io_context to let write_pump deliver the data
    io_ctx_.run();
    io_ctx_.restart();

    std::vector<uint8_t> received(data.size());
    boost::asio::read(client, boost::asio::buffer(received));
    EXPECT_EQ(received, data);

    client.close();
}

TEST_F(SessionTest, EnqueueWriteBufferFull)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

    // Fill the queue to max_queue_depth_ (default: 256).
    // Don't run io_context so write_pump doesn't drain the queue.
    constexpr size_t kDefaultMaxQueueDepth = 256;
    for (size_t i = 0; i < kDefaultMaxQueueDepth; ++i)
    {
        auto result = session->enqueue_write({0x01});
        ASSERT_TRUE(result.has_value()) << "enqueue failed at i=" << i;
    }

    // 257th enqueue should fail with BufferFull
    auto overflow = session->enqueue_write({0x02});
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error(), ErrorCode::BufferFull);

    // Clean up: drain the queue so session destruction is clean
    io_ctx_.run();
    io_ctx_.restart();
    client.close();
}

TEST_F(SessionTest, EnqueueWriteAfterClose)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

    session->close();
    EXPECT_EQ(session->state(), Session::State::Closed);

    auto result = session->enqueue_write({0x01, 0x02});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::SessionClosed);

    client.close();
}

TEST_F(SessionTest, EnqueueWriteRawAndReceive)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

    std::vector<uint8_t> data = {0xCA, 0xFE, 0xBA, 0xBE};
    std::span<const uint8_t> data_span(data);
    auto result = session->enqueue_write_raw(data_span);
    ASSERT_TRUE(result.has_value());

    // Run io_context to let write_pump deliver the data
    io_ctx_.run();
    io_ctx_.restart();

    std::vector<uint8_t> received(data.size());
    boost::asio::read(client, boost::asio::buffer(received));
    EXPECT_EQ(received, data);

    client.close();
}

TEST_F(SessionTest, EnqueueWriteFIFOOrder)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

    // Enqueue 3 messages with distinct patterns
    std::vector<uint8_t> msg1 = {0x01, 0x01, 0x01, 0x01};
    std::vector<uint8_t> msg2 = {0x02, 0x02, 0x02, 0x02};
    std::vector<uint8_t> msg3 = {0x03, 0x03, 0x03, 0x03};

    ASSERT_TRUE(session->enqueue_write(msg1).has_value());
    ASSERT_TRUE(session->enqueue_write(msg2).has_value());
    ASSERT_TRUE(session->enqueue_write(msg3).has_value());

    // Run io_context to let write_pump deliver all messages
    io_ctx_.run();
    io_ctx_.restart();

    // Read all 12 bytes and verify FIFO order
    std::vector<uint8_t> received(12);
    boost::asio::read(client, boost::asio::buffer(received));

    std::vector<uint8_t> expected;
    expected.insert(expected.end(), msg1.begin(), msg1.end());
    expected.insert(expected.end(), msg2.begin(), msg2.end());
    expected.insert(expected.end(), msg3.begin(), msg3.end());
    EXPECT_EQ(received, expected);

    client.close();
}

// --- Concurrency Tests ---

class SessionConcurrencyTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        io_ctx_.restart();
    }

    boost::asio::io_context io_ctx_;
};

TEST_F(SessionConcurrencyTest, ConcurrentAsyncSendAllDelivered)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

    // Two distinct payloads
    std::vector<uint8_t> payload_a = {0xAA, 0xBB, 0xCC, 0xDD};
    std::vector<uint8_t> payload_b = {0x11, 0x22, 0x33, 0x44};

    WireHeader header_a{.msg_id = 0x0001, .body_size = static_cast<uint32_t>(payload_a.size()), .reserved = {}};
    WireHeader header_b{.msg_id = 0x0002, .body_size = static_cast<uint32_t>(payload_b.size()), .reserved = {}};

    // Track results from each coroutine
    Result<void> result_a = error(ErrorCode::SendFailed);
    Result<void> result_b = error(ErrorCode::SendFailed);

    // Spawn two concurrent async_send coroutines
    boost::asio::co_spawn(
        io_ctx_, [&]() -> awaitable<void> { result_a = co_await session->async_send(header_a, payload_a); },
        boost::asio::detached);

    boost::asio::co_spawn(
        io_ctx_, [&]() -> awaitable<void> { result_b = co_await session->async_send(header_b, payload_b); },
        boost::asio::detached);

    // Run io_context to completion — both coroutines + write_pump execute
    io_ctx_.run();
    io_ctx_.restart();

    // Both sends must succeed
    ASSERT_TRUE(result_a.has_value()) << "async_send A failed";
    ASSERT_TRUE(result_b.has_value()) << "async_send B failed";

    // Read both frames from the client side
    constexpr size_t kFrameSize = WireHeader::SIZE + 4; // header + 4 bytes payload
    std::vector<uint8_t> received(kFrameSize * 2);
    boost::asio::read(client, boost::asio::buffer(received));

    // Parse both frames — order depends on scheduling, so check both are present
    auto frame1 = WireHeader::parse(std::span<const uint8_t>(received.data(), kFrameSize));
    auto frame2 = WireHeader::parse(std::span<const uint8_t>(received.data() + kFrameSize, kFrameSize));
    ASSERT_TRUE(frame1.has_value());
    ASSERT_TRUE(frame2.has_value());

    // Extract payloads from received data
    std::vector<uint8_t> body1(received.begin() + WireHeader::SIZE, received.begin() + kFrameSize);
    std::vector<uint8_t> body2(received.begin() + kFrameSize + WireHeader::SIZE, received.end());

    // Both frames must be present (order may vary)
    bool order_ab = (frame1->msg_id == 0x0001 && frame2->msg_id == 0x0002);
    bool order_ba = (frame1->msg_id == 0x0002 && frame2->msg_id == 0x0001);
    ASSERT_TRUE(order_ab || order_ba) << "Expected both msg_ids (0x0001, 0x0002), got " << frame1->msg_id << " and "
                                      << frame2->msg_id;

    if (order_ab)
    {
        EXPECT_EQ(body1, payload_a);
        EXPECT_EQ(body2, payload_b);
    }
    else
    {
        EXPECT_EQ(body1, payload_b);
        EXPECT_EQ(body2, payload_a);
    }

    client.close();
}

TEST_F(SessionConcurrencyTest, EnqueueWriteMaxDepthReturnsBufferFull)
{
    auto [server, client] = make_socket_pair(io_ctx_);

    // Small max_queue_depth for fast testing
    constexpr size_t kMaxDepth = 4;
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0, 8192, kMaxDepth));

    // Don't run io_context so write_pump doesn't drain the queue
    for (size_t i = 0; i < kMaxDepth; ++i)
    {
        auto result = session->enqueue_write({static_cast<uint8_t>(i)});
        ASSERT_TRUE(result.has_value()) << "enqueue failed at i=" << i;
    }

    // (max_queue_depth + 1)th enqueue should fail with BufferFull
    auto overflow = session->enqueue_write({0xFF});
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error(), ErrorCode::BufferFull);

    // Clean up: drain the queue so session destruction is clean
    io_ctx_.run();
    io_ctx_.restart();
    client.close();
}
