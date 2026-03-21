// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/error_code.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/shared/protocols/websocket/websocket_protocol.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace apex::shared::protocols::websocket;
using apex::core::ErrorCode;
using apex::core::RingBuffer;

/// Helper: write a length-prefixed WebSocket message into a RingBuffer.
static void write_ws_message(RingBuffer& buf, const std::vector<uint8_t>& payload)
{
    uint32_t len = static_cast<uint32_t>(payload.size());
    auto wr = buf.writable();
    std::memcpy(wr.data(), &len, sizeof(len));
    if (!payload.empty())
        std::memcpy(wr.data() + sizeof(len), payload.data(), payload.size());
    buf.commit_write(sizeof(len) + payload.size());
}

TEST(WebSocketProtocol, DecodeCompleteMessage)
{
    RingBuffer buf(1024);
    std::vector<uint8_t> payload = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"
    write_ws_message(buf, payload);

    auto result = WebSocketProtocol::try_decode(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_data, payload);
    EXPECT_TRUE(result->is_binary);
    EXPECT_FALSE(result->is_text);
}

TEST(WebSocketProtocol, DecodePartialHeader)
{
    RingBuffer buf(1024);

    // Write only 2 bytes (less than 4-byte length prefix)
    auto wr = buf.writable();
    wr[0] = 0x05;
    wr[1] = 0x00;
    buf.commit_write(2);

    auto result = WebSocketProtocol::try_decode(buf);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::InsufficientData);
}

TEST(WebSocketProtocol, DecodePartialPayload)
{
    RingBuffer buf(1024);

    // Write header claiming 10 bytes, but only provide 3 bytes of payload
    uint32_t claimed_len = 10;
    auto wr = buf.writable();
    std::memcpy(wr.data(), &claimed_len, sizeof(claimed_len));
    wr[4] = 0xAA;
    wr[5] = 0xBB;
    wr[6] = 0xCC;
    buf.commit_write(sizeof(claimed_len) + 3); // 7 bytes total, need 14

    auto result = WebSocketProtocol::try_decode(buf);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::InsufficientData);
}

TEST(WebSocketProtocol, ConsumeAdvancesBuffer)
{
    RingBuffer buf(1024);
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    write_ws_message(buf, payload);

    const size_t readable_before = buf.readable_size();
    EXPECT_EQ(readable_before, sizeof(uint32_t) + payload.size());

    auto result = WebSocketProtocol::try_decode(buf);
    ASSERT_TRUE(result.has_value());

    WebSocketProtocol::consume_frame(buf, *result);
    EXPECT_EQ(buf.readable_size(), 0u);
}

TEST(WebSocketProtocol, EmptyPayload)
{
    RingBuffer buf(1024);
    std::vector<uint8_t> empty_payload;
    write_ws_message(buf, empty_payload);

    auto result = WebSocketProtocol::try_decode(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->payload_data.empty());
    EXPECT_EQ(result->payload().size(), 0u);
}

TEST(WebSocketProtocol, MultipleFramesInBuffer)
{
    RingBuffer buf(4096);

    std::vector<uint8_t> msg1 = {0xAA, 0xBB};
    std::vector<uint8_t> msg2 = {0xCC, 0xDD, 0xEE};

    write_ws_message(buf, msg1);
    write_ws_message(buf, msg2);

    // Extract first message
    auto r1 = WebSocketProtocol::try_decode(buf);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->payload_data, msg1);
    WebSocketProtocol::consume_frame(buf, *r1);

    // Extract second message
    auto r2 = WebSocketProtocol::try_decode(buf);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->payload_data, msg2);
    WebSocketProtocol::consume_frame(buf, *r2);

    // Buffer should be empty now
    EXPECT_EQ(buf.readable_size(), 0u);
}

TEST(WebSocketProtocol, MaxMessageSizeExceeded)
{
    RingBuffer buf(64);

    // Write a header claiming > 1MB payload
    uint32_t oversized_len = 2 * 1024 * 1024; // 2MB > 1MB max
    auto wr = buf.writable();
    std::memcpy(wr.data(), &oversized_len, sizeof(oversized_len));
    buf.commit_write(sizeof(oversized_len));

    auto result = WebSocketProtocol::try_decode(buf);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::InvalidMessage);
}
