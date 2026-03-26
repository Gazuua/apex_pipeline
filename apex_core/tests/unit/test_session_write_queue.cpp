// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>

#include "../test_helpers.hpp"

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

/// Write queue 회귀 + 엣지 케이스 테스트.
/// C-prep-3에서 작성한 기본 4TC는 test_session.cpp에 유지.
/// 이 파일은 Phase 2 API 변경 후 호환성 + 추가 엣지 케이스.
class SessionWriteQueueRegressionTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        io_ctx_.restart();
    }

    boost::asio::io_context io_ctx_;
};

// TC1: enqueue_write_raw + enqueue_write 혼합 사용 — 순서 보장
TEST_F(SessionWriteQueueRegressionTest, MixedEnqueueOrder)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), make_tcp_socket(std::move(server)), 0));

    std::vector<uint8_t> msg1 = {0x01, 0x01};
    std::vector<uint8_t> msg2 = {0x02, 0x02};
    std::vector<uint8_t> msg3 = {0x03, 0x03};

    ASSERT_TRUE(session->enqueue_write(msg1).has_value());
    ASSERT_TRUE(session->enqueue_write_raw(std::span<const uint8_t>(msg2)).has_value());
    ASSERT_TRUE(session->enqueue_write(msg3).has_value());

    io_ctx_.run();
    io_ctx_.restart();

    std::vector<uint8_t> received(6);
    boost::asio::read(client, boost::asio::buffer(received));

    std::vector<uint8_t> expected;
    expected.insert(expected.end(), msg1.begin(), msg1.end());
    expected.insert(expected.end(), msg2.begin(), msg2.end());
    expected.insert(expected.end(), msg3.begin(), msg3.end());
    EXPECT_EQ(received, expected);

    client.close();
}

// TC2: 빈 데이터 enqueue — 정상 처리
TEST_F(SessionWriteQueueRegressionTest, EnqueueEmptyData)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), make_tcp_socket(std::move(server)), 0));

    // 빈 벡터 enqueue
    auto result = session->enqueue_write({});
    ASSERT_TRUE(result.has_value());

    // 후속 데이터 전송으로 빈 메시지 이후에도 정상 동작 확인
    std::vector<uint8_t> msg = {0xFF};
    ASSERT_TRUE(session->enqueue_write(msg).has_value());

    io_ctx_.run();
    io_ctx_.restart();

    std::vector<uint8_t> received(1);
    boost::asio::read(client, boost::asio::buffer(received));
    EXPECT_EQ(received[0], 0xFF);

    client.close();
}

// TC3: 대량 enqueue 후 drain — 모든 데이터 수신
TEST_F(SessionWriteQueueRegressionTest, BulkEnqueueDrains)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), make_tcp_socket(std::move(server)), 0));

    constexpr size_t count = 100;
    for (size_t i = 0; i < count; ++i)
    {
        auto result = session->enqueue_write({static_cast<uint8_t>(i)});
        ASSERT_TRUE(result.has_value()) << "enqueue failed at i=" << i;
    }

    io_ctx_.run();
    io_ctx_.restart();

    std::vector<uint8_t> received(count);
    boost::asio::read(client, boost::asio::buffer(received));

    for (size_t i = 0; i < count; ++i)
    {
        EXPECT_EQ(received[i], static_cast<uint8_t>(i)) << "Mismatch at index " << i;
    }

    client.close();
}

// TC4: 프로토콜 마이그레이션 호환성 — WireHeader 프레임을 enqueue_write로 전송
TEST_F(SessionWriteQueueRegressionTest, EnqueueWireHeaderFrame)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), make_tcp_socket(std::move(server)), 0));

    // WireHeader + payload를 enqueue_write로 전송 가능한지 확인
    std::vector<uint8_t> payload = {0xCA, 0xFE};
    WireHeader header{.msg_id = 0x0042, .body_size = static_cast<uint32_t>(payload.size()), .reserved = {}};
    auto hdr_bytes = header.serialize();

    std::vector<uint8_t> frame(hdr_bytes.begin(), hdr_bytes.end());
    frame.insert(frame.end(), payload.begin(), payload.end());

    auto result = session->enqueue_write(std::move(frame));
    ASSERT_TRUE(result.has_value());

    io_ctx_.run();
    io_ctx_.restart();

    // 수신 측에서 WireHeader 파싱
    std::vector<uint8_t> received(WireHeader::SIZE + payload.size());
    boost::asio::read(client, boost::asio::buffer(received));

    auto parsed = WireHeader::parse(received);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->msg_id, 0x0042);
    EXPECT_EQ(parsed->body_size, 2u);

    // payload 검증
    EXPECT_EQ(received[WireHeader::SIZE], 0xCA);
    EXPECT_EQ(received[WireHeader::SIZE + 1], 0xFE);

    client.close();
}

// TC5: close된 세션에 enqueue_write -- SessionClosed 에러
TEST_F(SessionWriteQueueRegressionTest, EnqueueAfterCloseReturnsError)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), make_tcp_socket(std::move(server)), 0));

    session->close();
    ASSERT_FALSE(session->is_open());

    auto result = session->enqueue_write({0x01, 0x02});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::core::ErrorCode::SessionClosed);

    // enqueue_write_raw도 동일
    std::vector<uint8_t> data = {0x03};
    auto result2 = session->enqueue_write_raw(std::span<const uint8_t>(data));
    ASSERT_FALSE(result2.has_value());
    EXPECT_EQ(result2.error(), apex::core::ErrorCode::SessionClosed);

    client.close();
}

// --- async_send / async_send_raw → write_pump 경유 테스트 (BACKLOG-22) ---

// TC6: async_send_raw가 write_pump를 경유하여 데이터 전송
TEST_F(SessionWriteQueueRegressionTest, AsyncSendRawRoutesViaWritePump)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), make_tcp_socket(std::move(server)), 0));

    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    auto result = run_coro(io_ctx_, session->async_send_raw(data));
    ASSERT_TRUE(result.has_value());

    std::vector<uint8_t> received(data.size());
    boost::asio::read(client, boost::asio::buffer(received));
    EXPECT_EQ(received, data);

    client.close();
}

// TC7: async_send가 write_pump를 경유하여 WireHeader+payload 전송
TEST_F(SessionWriteQueueRegressionTest, AsyncSendRoutesViaWritePump)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), make_tcp_socket(std::move(server)), 0));

    std::vector<uint8_t> payload = {0xCA, 0xFE};
    WireHeader header{.msg_id = 0x0099, .body_size = static_cast<uint32_t>(payload.size()), .reserved = {}};

    auto result = run_coro(io_ctx_, session->async_send(header, payload));
    ASSERT_TRUE(result.has_value());

    // Verify received data contains correct WireHeader + payload
    std::vector<uint8_t> received(WireHeader::SIZE + payload.size());
    boost::asio::read(client, boost::asio::buffer(received));

    auto parsed = WireHeader::parse(received);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->msg_id, 0x0099);
    EXPECT_EQ(parsed->body_size, 2u);
    EXPECT_EQ(received[WireHeader::SIZE], 0xCA);
    EXPECT_EQ(received[WireHeader::SIZE + 1], 0xFE);

    client.close();
}

// TC8: async_send_raw + enqueue_write 연속 호출 — FIFO 순서 보장
// (같은 코루틴 내에서 async_send_raw 완료 후 enqueue_write 호출)
TEST_F(SessionWriteQueueRegressionTest, AsyncSendThenEnqueue_FIFOOrder)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), make_tcp_socket(std::move(server)), 0));

    // async_send_raw first, then enqueue_write
    std::vector<uint8_t> msg1 = {0xAA, 0xBB};
    std::vector<uint8_t> msg2 = {0xCC, 0xDD};

    auto coro = [&session, &msg1, &msg2]() -> awaitable<void> {
        auto r1 = co_await session->async_send_raw(msg1);
        EXPECT_TRUE(r1.has_value());

        // After async_send_raw completes, enqueue_write should work fine
        auto r2 = session->enqueue_write(msg2);
        EXPECT_TRUE(r2.has_value());
    };

    auto future = boost::asio::co_spawn(io_ctx_, coro(), boost::asio::use_future);
    io_ctx_.run();
    io_ctx_.restart();
    future.get();

    // Read both messages in order
    std::vector<uint8_t> received(4);
    boost::asio::read(client, boost::asio::buffer(received));

    std::vector<uint8_t> expected;
    expected.insert(expected.end(), msg1.begin(), msg1.end());
    expected.insert(expected.end(), msg2.begin(), msg2.end());
    EXPECT_EQ(received, expected);

    // Drain any remaining pump activity
    io_ctx_.run();
    io_ctx_.restart();

    client.close();
}

// TC9: async_send_raw on closed session returns SessionClosed
TEST_F(SessionWriteQueueRegressionTest, AsyncSendRawAfterCloseReturnsError)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), make_tcp_socket(std::move(server)), 0));

    session->close();
    ASSERT_FALSE(session->is_open());

    std::vector<uint8_t> data = {0x01, 0x02};
    auto result = run_coro(io_ctx_, session->async_send_raw(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::SessionClosed);

    client.close();
}

// TC10: async_send on closed session returns SessionClosed
TEST_F(SessionWriteQueueRegressionTest, AsyncSendAfterCloseReturnsError)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), make_tcp_socket(std::move(server)), 0));

    session->close();
    ASSERT_FALSE(session->is_open());

    std::vector<uint8_t> payload = {0x01};
    WireHeader header{.msg_id = 1, .body_size = 1, .reserved = {}};
    auto result = run_coro(io_ctx_, session->async_send(header, payload));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::SessionClosed);

    client.close();
}

// TC11: enqueue_write 여러 개 + async_send_raw — 모든 데이터가 올바르게 전달
TEST_F(SessionWriteQueueRegressionTest, MixedEnqueueAndAsyncSend)
{
    auto [server, client] = make_socket_pair(io_ctx_);
    SessionPtr session(new Session(make_session_id(1), make_tcp_socket(std::move(server)), 0));

    // enqueue_write (fire-and-forget) then async_send_raw (awaitable)
    std::vector<uint8_t> msg1 = {0x11, 0x22};
    std::vector<uint8_t> msg2 = {0x33, 0x44};

    ASSERT_TRUE(session->enqueue_write(msg1).has_value());

    // async_send_raw will enqueue behind msg1 and wait for its own completion
    auto result = run_coro(io_ctx_, session->async_send_raw(msg2));
    ASSERT_TRUE(result.has_value());

    // Both messages should have been sent in order
    std::vector<uint8_t> received(4);
    boost::asio::read(client, boost::asio::buffer(received));

    std::vector<uint8_t> expected;
    expected.insert(expected.end(), msg1.begin(), msg1.end());
    expected.insert(expected.end(), msg2.begin(), msg2.end());
    EXPECT_EQ(received, expected);

    client.close();
}
