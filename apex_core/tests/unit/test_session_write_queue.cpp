#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>

#include "../test_helpers.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>

#include <gtest/gtest.h>

using namespace apex::core;
using apex::test::make_socket_pair;
using apex::test::run_coro;

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
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

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
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

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
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

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
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

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
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0));

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
