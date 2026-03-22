// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/error_code.hpp>
#include <apex/core/protocol.hpp>
#include <apex/core/wire_header.hpp>
#include <apex/shared/protocols/tcp/tcp_binary_protocol.hpp>

#include <gtest/gtest.h>

#include <cstring>

using namespace apex::core;
using apex::shared::protocols::tcp::TcpBinaryProtocol;

class TcpBinaryProtocolTest : public ::testing::Test
{
  protected:
    RingBuffer buf_{4096};

    void write_frame(uint32_t msg_id, const std::vector<uint8_t>& body)
    {
        WireHeader hdr{.msg_id = msg_id, .body_size = static_cast<uint32_t>(body.size()), .reserved = {}};
        std::array<uint8_t, WireHeader::SIZE> raw{};
        hdr.serialize(raw);

        auto w = buf_.writable();
        std::memcpy(w.data(), raw.data(), raw.size());
        buf_.commit_write(raw.size());

        if (!body.empty())
        {
            auto w2 = buf_.writable();
            std::memcpy(w2.data(), body.data(), body.size());
            buf_.commit_write(body.size());
        }
    }
};

TEST_F(TcpBinaryProtocolTest, DecodeMatchesFrameCodec)
{
    std::vector<uint8_t> body = {0xDE, 0xAD, 0xBE, 0xEF};
    write_frame(0x0042, body);

    auto result = TcpBinaryProtocol::try_decode(buf_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->header.msg_id, 0x0042);
    EXPECT_EQ(result->header.body_size, 4u);
    EXPECT_EQ(result->payload().size(), 4u);
    EXPECT_EQ(result->payload()[0], 0xDE);
}

TEST_F(TcpBinaryProtocolTest, ConsumeFrameAdvancesBuffer)
{
    std::vector<uint8_t> body = {0x01, 0x02};
    write_frame(0x0001, body);

    auto frame = TcpBinaryProtocol::try_decode(buf_);
    ASSERT_TRUE(frame.has_value());

    size_t before = buf_.readable_size();
    TcpBinaryProtocol::consume_frame(buf_, *frame);
    size_t after = buf_.readable_size();

    EXPECT_LT(after, before);
    EXPECT_EQ(after, 0u);
}

TEST_F(TcpBinaryProtocolTest, EmptyBufferReturnsError)
{
    auto result = TcpBinaryProtocol::try_decode(buf_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::InsufficientData);
}

TEST_F(TcpBinaryProtocolTest, SatisfiesProtocolConcept)
{
    static_assert(Protocol<TcpBinaryProtocol>);
}

TEST_F(TcpBinaryProtocolTest, FrameTypeMatchesCoreFrame)
{
    static_assert(std::is_same_v<TcpBinaryProtocol::Frame, apex::core::Frame>);
}

TEST_F(TcpBinaryProtocolTest, PartialBodyReturnsInsufficientData)
{
    // 헤더에 body_size=100 기록하지만 실제로 10바이트만 write
    WireHeader hdr{.msg_id = 0x0042, .body_size = 100, .reserved = {}};
    std::array<uint8_t, WireHeader::SIZE> raw{};
    hdr.serialize(raw);

    auto w = buf_.writable();
    std::memcpy(w.data(), raw.data(), raw.size());
    buf_.commit_write(raw.size());

    // body 10바이트만 기록 (100바이트 중)
    std::vector<uint8_t> partial_body(10, 0xAA);
    auto w2 = buf_.writable();
    std::memcpy(w2.data(), partial_body.data(), partial_body.size());
    buf_.commit_write(partial_body.size());

    auto result = TcpBinaryProtocol::try_decode(buf_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::InsufficientData);
}

TEST_F(TcpBinaryProtocolTest, BodyTooLargeReturnsError)
{
    // body_size를 MAX_BODY_SIZE 초과 값으로 설정 (16MB + 1)
    WireHeader hdr{.msg_id = 0x0042, .body_size = WireHeader::MAX_BODY_SIZE + 1, .reserved = {}};
    std::array<uint8_t, WireHeader::SIZE> raw{};
    hdr.serialize(raw);

    auto w = buf_.writable();
    std::memcpy(w.data(), raw.data(), raw.size());
    buf_.commit_write(raw.size());

    auto result = TcpBinaryProtocol::try_decode(buf_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::BufferFull);
}

TEST_F(TcpBinaryProtocolTest, UnsupportedVersionReturnsError)
{
    // version 필드를 잘못된 값으로 설정하여 UnsupportedProtocolVersion 에러 확인
    WireHeader hdr{.version = 99, .msg_id = 0x0042, .body_size = 4, .reserved = {}};
    std::array<uint8_t, WireHeader::SIZE> raw{};
    hdr.serialize(raw);

    auto w = buf_.writable();
    std::memcpy(w.data(), raw.data(), raw.size());
    buf_.commit_write(raw.size());

    auto result = TcpBinaryProtocol::try_decode(buf_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::UnsupportedProtocolVersion);
}

TEST_F(TcpBinaryProtocolTest, ConsecutiveDecodeConsumeCycle)
{
    // 2개 프레임 연속 write
    write_frame(0x0001, {0xAA});
    write_frame(0x0002, {0xBB});

    // 첫 번째 decode+consume
    auto result1 = TcpBinaryProtocol::try_decode(buf_);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1->header.msg_id, 0x0001);
    ASSERT_EQ(result1->payload().size(), 1u);
    EXPECT_EQ(result1->payload()[0], 0xAA);
    TcpBinaryProtocol::consume_frame(buf_, *result1);

    // 두 번째 decode+consume
    auto result2 = TcpBinaryProtocol::try_decode(buf_);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2->header.msg_id, 0x0002);
    ASSERT_EQ(result2->payload().size(), 1u);
    EXPECT_EQ(result2->payload()[0], 0xBB);
    TcpBinaryProtocol::consume_frame(buf_, *result2);

    // 모든 프레임 소비 후 버퍼 비어있는지 명시 검증
    EXPECT_EQ(buf_.readable_size(), 0u);

    // 세 번째 decode → InsufficientData
    auto result3 = TcpBinaryProtocol::try_decode(buf_);
    ASSERT_FALSE(result3.has_value());
    EXPECT_EQ(result3.error(), ErrorCode::InsufficientData);
}

TEST_F(TcpBinaryProtocolTest, ZeroBodySizeDecodesSuccessfully)
{
    // 경계값: body_size=0 프레임 — 헤더만 존재, payload 비어있음
    write_frame(0x0042, {});

    auto result = TcpBinaryProtocol::try_decode(buf_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->header.msg_id, 0x0042);
    EXPECT_EQ(result->header.body_size, 0u);
    EXPECT_TRUE(result->payload().empty());

    TcpBinaryProtocol::consume_frame(buf_, *result);
    EXPECT_EQ(buf_.readable_size(), 0u);
}

TEST_F(TcpBinaryProtocolTest, HeaderOnlyInsufficientData)
{
    // 경계값: 헤더 바이트 수보다 적은 데이터 (예: 5바이트)
    std::vector<uint8_t> partial(5, 0x00);
    auto w = buf_.writable();
    std::memcpy(w.data(), partial.data(), partial.size());
    buf_.commit_write(partial.size());

    auto result = TcpBinaryProtocol::try_decode(buf_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::InsufficientData);
}
