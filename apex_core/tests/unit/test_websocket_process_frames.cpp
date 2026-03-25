// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

/// WebSocket process_frames else 분기 검증.
/// ConnectionHandler::process_frames의 `if constexpr (requires { frame.header.msg_id; })` else 측은
/// WebSocket-style 프레임(payload 첫 4바이트 = msg_id)을 처리한다.
///
/// ConnectionHandler는 코루틴 + SessionManager + Listener 의존이 깊어 직접 인스턴스화가 어려우므로,
/// else 분기의 핵심 로직(ntohl 바이트 오더 변환 + 4바이트 미만 프레임 검증)을
/// 동일 로직의 독립 함수로 추출하여 테스트한다.

#include <gtest/gtest.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace
{

/// WebSocket-style msg_id 추출 로직 — connection_handler.hpp else 분기와 동일.
/// 반환: {msg_id, payload_span} 또는 nullopt (4바이트 미만).
struct WebSocketExtractResult
{
    uint32_t msg_id;
    std::span<const uint8_t> payload;
};

std::optional<WebSocketExtractResult> extract_ws_msg_id(std::span<const uint8_t> raw)
{
    if (raw.size() < sizeof(uint32_t))
    {
        return std::nullopt; // frame too small for msg_id
    }
    uint32_t raw_id = 0;
    std::memcpy(&raw_id, raw.data(), sizeof(uint32_t));
    uint32_t msg_id = ntohl(raw_id); // big-endian → host
    auto payload = std::span<const uint8_t>(raw.data() + sizeof(uint32_t), raw.size() - sizeof(uint32_t));
    return WebSocketExtractResult{msg_id, payload};
}

} // namespace

// TC1: WebSocket 프레임이 4바이트 미만일 때 early-close 경로
TEST(WebSocketProcessFrames, FrameTooSmallReturnsNullopt)
{
    // 빈 프레임
    {
        std::vector<uint8_t> empty;
        auto result = extract_ws_msg_id(empty);
        EXPECT_FALSE(result.has_value()) << "Empty frame should fail extraction";
    }

    // 1바이트
    {
        std::vector<uint8_t> one = {0x01};
        auto result = extract_ws_msg_id(one);
        EXPECT_FALSE(result.has_value()) << "1-byte frame should fail extraction";
    }

    // 2바이트
    {
        std::vector<uint8_t> two = {0x01, 0x02};
        auto result = extract_ws_msg_id(two);
        EXPECT_FALSE(result.has_value()) << "2-byte frame should fail extraction";
    }

    // 3바이트
    {
        std::vector<uint8_t> three = {0x01, 0x02, 0x03};
        auto result = extract_ws_msg_id(three);
        EXPECT_FALSE(result.has_value()) << "3-byte frame should fail extraction";
    }
}

// TC2: 정확히 4바이트 — msg_id만, payload 없음
TEST(WebSocketProcessFrames, ExactlyFourBytesExtractsMsgIdOnly)
{
    // msg_id = 0x00000042 in big-endian
    uint32_t network_id = htonl(0x00000042);
    std::vector<uint8_t> data(sizeof(uint32_t));
    std::memcpy(data.data(), &network_id, sizeof(uint32_t));

    auto result = extract_ws_msg_id(data);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_id, 0x00000042u);
    EXPECT_TRUE(result->payload.empty()) << "4-byte frame should have empty payload";
}

// TC3: ntohl 바이트 오더 변환 검증 — big-endian 인코딩이 올바르게 디코딩되는지
TEST(WebSocketProcessFrames, NtohlByteOrderConversion)
{
    // 다양한 msg_id 값으로 round-trip 검증
    const uint32_t test_ids[] = {0x00000000, 0x00000001, 0x0000FFFF, 0xDEADBEEF, 0xFFFFFFFF, 0x12345678};

    for (uint32_t expected_id : test_ids)
    {
        uint32_t network_id = htonl(expected_id);
        std::vector<uint8_t> data(sizeof(uint32_t) + 4); // 4바이트 msg_id + 4바이트 payload
        std::memcpy(data.data(), &network_id, sizeof(uint32_t));
        data[4] = 0xCA;
        data[5] = 0xFE;
        data[6] = 0xBA;
        data[7] = 0xBE;

        auto result = extract_ws_msg_id(data);
        ASSERT_TRUE(result.has_value()) << "Extraction failed for msg_id=0x" << std::hex << expected_id;
        EXPECT_EQ(result->msg_id, expected_id) << "ntohl conversion failed for msg_id=0x" << std::hex << expected_id;
        EXPECT_EQ(result->payload.size(), 4u) << "Payload size mismatch for msg_id=0x" << std::hex << expected_id;
        EXPECT_EQ(result->payload[0], 0xCA);
        EXPECT_EQ(result->payload[1], 0xFE);
    }
}

// TC4: htonl/ntohl round-trip 일관성
TEST(WebSocketProcessFrames, HtonlNtohlRoundTrip)
{
    for (uint32_t i = 0; i < 1000; ++i)
    {
        uint32_t original = i * 4294967u; // 넓은 범위의 값
        uint32_t network = htonl(original);
        uint32_t host = ntohl(network);
        EXPECT_EQ(host, original) << "Round-trip failed for value " << original;
    }
}

// TC5: 큰 payload 포함 프레임에서 payload 분리 정확성
TEST(WebSocketProcessFrames, LargePayloadSplitCorrectly)
{
    constexpr size_t payload_size = 1024;
    uint32_t network_id = htonl(0x0099);

    std::vector<uint8_t> data(sizeof(uint32_t) + payload_size);
    std::memcpy(data.data(), &network_id, sizeof(uint32_t));
    for (size_t i = 0; i < payload_size; ++i)
        data[sizeof(uint32_t) + i] = static_cast<uint8_t>(i & 0xFF);

    auto result = extract_ws_msg_id(data);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_id, 0x0099u);
    ASSERT_EQ(result->payload.size(), payload_size);

    for (size_t i = 0; i < payload_size; ++i)
    {
        EXPECT_EQ(result->payload[i], static_cast<uint8_t>(i & 0xFF)) << "Payload mismatch at index " << i;
    }
}
