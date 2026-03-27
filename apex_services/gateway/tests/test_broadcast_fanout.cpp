// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/broadcast_fanout.hpp>

#include <apex/core/wire_header.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

namespace apex::gateway::test
{

class BroadcastFanoutBuildWireFrameTest : public ::testing::Test
{
  protected:
    // Helper: create a Redis Pub/Sub message with big-endian msg_id + payload
    static std::vector<uint8_t> make_pubsub_message(uint32_t msg_id, std::span<const uint8_t> payload = {})
    {
        std::vector<uint8_t> msg(PUBSUB_MSG_ID_SIZE + payload.size());
        msg[0] = static_cast<uint8_t>((msg_id >> 24) & 0xFF);
        msg[1] = static_cast<uint8_t>((msg_id >> 16) & 0xFF);
        msg[2] = static_cast<uint8_t>((msg_id >> 8) & 0xFF);
        msg[3] = static_cast<uint8_t>(msg_id & 0xFF);
        if (!payload.empty())
        {
            std::memcpy(msg.data() + PUBSUB_MSG_ID_SIZE, payload.data(), payload.size());
        }
        return msg;
    }
};

// 정상 케이스: 4바이트 msg_id + payload → WireHeader v2 프레임 생성
TEST_F(BroadcastFanoutBuildWireFrameTest, NormalMessageWithPayload)
{
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04, 0x05};
    auto msg = make_pubsub_message(42, payload);

    auto frame = BroadcastFanout::build_wire_frame(msg);

    ASSERT_FALSE(frame.empty());
    ASSERT_EQ(frame.size(), apex::core::WireHeader::SIZE + payload.size());

    // Parse the WireHeader from frame
    auto wh_result =
        apex::core::WireHeader::parse(std::span<const uint8_t>(frame.data(), apex::core::WireHeader::SIZE));
    ASSERT_TRUE(wh_result.has_value());
    EXPECT_EQ(wh_result->msg_id, 42u);
    EXPECT_EQ(wh_result->body_size, payload.size());
    EXPECT_EQ(wh_result->version, apex::core::WireHeader::CURRENT_VERSION);
    EXPECT_EQ(wh_result->flags, 0u);

    // Verify payload is preserved after WireHeader
    auto frame_payload = std::span<const uint8_t>(frame.data() + apex::core::WireHeader::SIZE, payload.size());
    EXPECT_TRUE(std::equal(frame_payload.begin(), frame_payload.end(), payload.begin()));
}

// 정상 케이스: 4바이트 msg_id만 (payload 없음)
TEST_F(BroadcastFanoutBuildWireFrameTest, MsgIdOnlyNoPayload)
{
    auto msg = make_pubsub_message(100);

    auto frame = BroadcastFanout::build_wire_frame(msg);

    ASSERT_FALSE(frame.empty());
    ASSERT_EQ(frame.size(), apex::core::WireHeader::SIZE); // header only, no payload

    auto wh_result =
        apex::core::WireHeader::parse(std::span<const uint8_t>(frame.data(), apex::core::WireHeader::SIZE));
    ASSERT_TRUE(wh_result.has_value());
    EXPECT_EQ(wh_result->msg_id, 100u);
    EXPECT_EQ(wh_result->body_size, 0u);
}

// 빈 메시지 → msg_id=0, empty payload (heartbeat/signal)
TEST_F(BroadcastFanoutBuildWireFrameTest, EmptyMessageHeartbeat)
{
    std::span<const uint8_t> empty;
    auto frame = BroadcastFanout::build_wire_frame(empty);

    ASSERT_FALSE(frame.empty());
    ASSERT_EQ(frame.size(), apex::core::WireHeader::SIZE);

    auto wh_result =
        apex::core::WireHeader::parse(std::span<const uint8_t>(frame.data(), apex::core::WireHeader::SIZE));
    ASSERT_TRUE(wh_result.has_value());
    EXPECT_EQ(wh_result->msg_id, 0u);
    EXPECT_EQ(wh_result->body_size, 0u);
}

// Malformed: 1바이트 메시지 → 빈 벡터 반환
TEST_F(BroadcastFanoutBuildWireFrameTest, MalformedOneByte)
{
    std::vector<uint8_t> msg = {0x42};
    auto frame = BroadcastFanout::build_wire_frame(msg);
    EXPECT_TRUE(frame.empty());
}

// Malformed: 2바이트 메시지 → 빈 벡터 반환
TEST_F(BroadcastFanoutBuildWireFrameTest, MalformedTwoBytes)
{
    std::vector<uint8_t> msg = {0x00, 0x01};
    auto frame = BroadcastFanout::build_wire_frame(msg);
    EXPECT_TRUE(frame.empty());
}

// Malformed: 3바이트 메시지 → 빈 벡터 반환
TEST_F(BroadcastFanoutBuildWireFrameTest, MalformedThreeBytes)
{
    std::vector<uint8_t> msg = {0x00, 0x01, 0x02};
    auto frame = BroadcastFanout::build_wire_frame(msg);
    EXPECT_TRUE(frame.empty());
}

// 빅엔디안 msg_id 바이트 순서 검증
TEST_F(BroadcastFanoutBuildWireFrameTest, BigEndianMsgIdParsing)
{
    // msg_id = 0x01020304 (big-endian)
    auto msg = make_pubsub_message(0x01020304);

    auto frame = BroadcastFanout::build_wire_frame(msg);
    ASSERT_FALSE(frame.empty());

    auto wh_result =
        apex::core::WireHeader::parse(std::span<const uint8_t>(frame.data(), apex::core::WireHeader::SIZE));
    ASSERT_TRUE(wh_result.has_value());
    EXPECT_EQ(wh_result->msg_id, 0x01020304u);
}

// 최대 msg_id (0xFFFFFFFF)
TEST_F(BroadcastFanoutBuildWireFrameTest, MaxMsgId)
{
    auto msg = make_pubsub_message(0xFFFFFFFF);

    auto frame = BroadcastFanout::build_wire_frame(msg);
    ASSERT_FALSE(frame.empty());

    auto wh_result =
        apex::core::WireHeader::parse(std::span<const uint8_t>(frame.data(), apex::core::WireHeader::SIZE));
    ASSERT_TRUE(wh_result.has_value());
    EXPECT_EQ(wh_result->msg_id, 0xFFFFFFFFu);
}

// 큰 payload
TEST_F(BroadcastFanoutBuildWireFrameTest, LargePayload)
{
    std::vector<uint8_t> payload(4096, 0xAB);
    auto msg = make_pubsub_message(7, payload);

    auto frame = BroadcastFanout::build_wire_frame(msg);
    ASSERT_FALSE(frame.empty());
    ASSERT_EQ(frame.size(), apex::core::WireHeader::SIZE + 4096);

    auto wh_result =
        apex::core::WireHeader::parse(std::span<const uint8_t>(frame.data(), apex::core::WireHeader::SIZE));
    ASSERT_TRUE(wh_result.has_value());
    EXPECT_EQ(wh_result->msg_id, 7u);
    EXPECT_EQ(wh_result->body_size, 4096u);
}

} // namespace apex::gateway::test
