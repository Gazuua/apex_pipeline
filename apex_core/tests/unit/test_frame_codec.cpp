#include <apex/core/frame_codec.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/wire_header.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace apex::core;

namespace
{

void write_to_buf(RingBuffer& buf, std::span<const uint8_t> data)
{
    auto writable = buf.writable();
    ASSERT_GE(writable.size(), data.size());
    std::memcpy(writable.data(), data.data(), data.size());
    buf.commit_write(data.size());
}

// Uses WireHeader::CURRENT_VERSION by default (set in WireHeader's designated initializer)
std::vector<uint8_t> build_frame(uint32_t msg_id, std::span<const uint8_t> payload)
{
    WireHeader h{.msg_id = msg_id, .body_size = static_cast<uint32_t>(payload.size()), .reserved = {}};
    auto header_bytes = h.serialize();
    std::vector<uint8_t> frame(header_bytes.begin(), header_bytes.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

} // anonymous namespace

TEST(FrameCodec, DecodeEmptyBuffer)
{
    RingBuffer buf(4096);
    auto result = FrameCodec::try_decode(buf);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::InsufficientData);
}

TEST(FrameCodec, DecodeIncompleteHeader)
{
    RingBuffer buf(4096);
    std::array<uint8_t, 5> partial{};
    write_to_buf(buf, partial);

    auto result = FrameCodec::try_decode(buf);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::InsufficientData);
}

TEST(FrameCodec, DecodeCompleteFrame)
{
    RingBuffer buf(4096);
    std::array<uint8_t, 4> payload{0xDE, 0xAD, 0xBE, 0xEF};
    auto frame_data = build_frame(42, payload);
    write_to_buf(buf, frame_data);

    auto result = FrameCodec::try_decode(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->header.msg_id, 42u);
    EXPECT_EQ(result->header.body_size, 4u);
    ASSERT_EQ(result->payload().size(), 4u);
    EXPECT_EQ(result->payload()[0], 0xDE);
    EXPECT_EQ(result->payload()[1], 0xAD);
    EXPECT_EQ(result->payload()[2], 0xBE);
    EXPECT_EQ(result->payload()[3], 0xEF);
}

TEST(FrameCodec, ConsumeFrame)
{
    RingBuffer buf(4096);
    std::array<uint8_t, 4> payload{0x01, 0x02, 0x03, 0x04};
    auto frame_data = build_frame(1, payload);
    write_to_buf(buf, frame_data);

    auto result = FrameCodec::try_decode(buf);
    ASSERT_TRUE(result.has_value());

    FrameCodec::consume_frame(buf, *result);
    EXPECT_EQ(buf.readable_size(), 0u);
}

TEST(FrameCodec, DecodeMultipleFrames)
{
    RingBuffer buf(4096);

    std::array<uint8_t, 2> payload1{0xAA, 0xBB};
    std::array<uint8_t, 3> payload2{0xCC, 0xDD, 0xEE};
    auto frame1 = build_frame(10, payload1);
    auto frame2 = build_frame(20, payload2);

    write_to_buf(buf, frame1);
    write_to_buf(buf, frame2);

    // Decode first frame
    auto r1 = FrameCodec::try_decode(buf);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->header.msg_id, 10u);
    EXPECT_EQ(r1->header.body_size, 2u);
    FrameCodec::consume_frame(buf, *r1);

    // Decode second frame
    auto r2 = FrameCodec::try_decode(buf);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->header.msg_id, 20u);
    EXPECT_EQ(r2->header.body_size, 3u);
    FrameCodec::consume_frame(buf, *r2);

    EXPECT_EQ(buf.readable_size(), 0u);
}

TEST(FrameCodec, DecodeHeaderPresentButBodyIncomplete)
{
    RingBuffer buf(4096);

    // Build a frame with body_size=100 but only write the header + partial body
    WireHeader h{.msg_id = 5, .body_size = 100, .reserved = {}};
    auto header_bytes = h.serialize();
    write_to_buf(buf, header_bytes);

    // Write only 10 bytes of the expected 100
    std::array<uint8_t, 10> partial_body{};
    write_to_buf(buf, partial_body);

    auto result = FrameCodec::try_decode(buf);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::InsufficientData);
}

TEST(FrameCodec, DecodeZeroBodyFrame)
{
    RingBuffer buf(4096);
    auto frame_data = build_frame(99, {});
    write_to_buf(buf, frame_data);

    auto result = FrameCodec::try_decode(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->header.msg_id, 99u);
    EXPECT_EQ(result->header.body_size, 0u);
    EXPECT_TRUE(result->payload().empty());
}

TEST(FrameCodec, EncodeToBuffer)
{
    std::array<uint8_t, 4> payload{0x01, 0x02, 0x03, 0x04};
    WireHeader h{.msg_id = 55, .body_size = 4, .reserved = {}};

    std::vector<uint8_t> out(WireHeader::SIZE + payload.size());
    auto written = FrameCodec::encode_to(out, h, payload);

    ASSERT_EQ(written, WireHeader::SIZE + payload.size());

    // Verify by parsing
    auto parsed = WireHeader::parse(std::span<const uint8_t>(out));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->msg_id, 55u);
    EXPECT_EQ(parsed->body_size, 4u);

    // Check payload bytes
    EXPECT_EQ(out[WireHeader::SIZE + 0], 0x01);
    EXPECT_EQ(out[WireHeader::SIZE + 1], 0x02);
    EXPECT_EQ(out[WireHeader::SIZE + 2], 0x03);
    EXPECT_EQ(out[WireHeader::SIZE + 3], 0x04);
}

// T5b: FrameCodec decode with BodyTooLarge header
TEST(FrameCodec, DecodeBodyTooLarge)
{
    RingBuffer buf(4096);

    // Build a header with body_size exceeding MAX_BODY_SIZE
    WireHeader h{.msg_id = 1, .body_size = WireHeader::MAX_BODY_SIZE + 1, .reserved = {}};
    // Manually serialize (serialize doesn't validate body_size)
    auto header_bytes = h.serialize();
    write_to_buf(buf, header_bytes);

    auto result = FrameCodec::try_decode(buf);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::BodyTooLarge);
}

// T7: encode_to with body_size mismatch
TEST(FrameCodec, EncodeToBodySizeMismatch)
{
    std::array<uint8_t, 4> payload{0x01, 0x02, 0x03, 0x04};
    WireHeader h{.msg_id = 1, .body_size = 100, .reserved = {}}; // mismatch: says 100, payload is 4

    std::vector<uint8_t> out(256);
    auto written = FrameCodec::encode_to(out, h, payload);
    EXPECT_EQ(written, 0u); // should reject
}

// T7b: encode() with body_size mismatch
TEST(FrameCodec, EncodeBodySizeMismatch)
{
    RingBuffer buf(4096);
    std::array<uint8_t, 4> payload{0x01, 0x02, 0x03, 0x04};
    WireHeader h{.msg_id = 1, .body_size = 10, .reserved = {}}; // mismatch

    EXPECT_FALSE(FrameCodec::encode(buf, h, payload));
    EXPECT_EQ(buf.readable_size(), 0u); // nothing written
}

TEST(FrameCodec, EncodeWrapAround)
{
    // Use a small RingBuffer to force wrap-around
    RingBuffer buf(64);

    // Write 50 bytes and consume to advance write/read positions near the end
    std::vector<uint8_t> filler(50, 0xAA);
    write_to_buf(buf, filler);
    buf.consume(50);

    // Now write_pos is at 50, read_pos is at 50. Remaining contiguous space = 14.
    // Encode a frame that wraps around the boundary
    std::array<uint8_t, 8> payload{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    WireHeader h{.msg_id = 42, .body_size = 8, .reserved = {}};

    // Total frame size = WireHeader::SIZE(12) + 8 = 20 bytes, which exceeds
    // the 14 bytes of contiguous space, requiring wrap-around
    ASSERT_TRUE(FrameCodec::encode(buf, h, payload));

    // Decode and verify the wrapped frame is correct
    auto result = FrameCodec::try_decode(buf);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->header.msg_id, 42u);
    EXPECT_EQ(result->header.body_size, 8u);
    ASSERT_EQ(result->payload().size(), 8u);
    for (size_t i = 0; i < 8; ++i)
    {
        EXPECT_EQ(result->payload()[i], payload[i]);
    }
}

TEST(FrameCodec, EncodeInsufficientSpace)
{
    // Create a small buffer and fill it almost completely
    RingBuffer buf(32);

    // Fill with data leaving insufficient space for a new frame
    std::vector<uint8_t> filler(30, 0xBB);
    write_to_buf(buf, filler);

    // Try to encode a frame that won't fit
    std::array<uint8_t, 4> payload{0x01, 0x02, 0x03, 0x04};
    WireHeader h{.msg_id = 1, .body_size = 4, .reserved = {}};
    // Total needed: WireHeader::SIZE(12) + 4 = 16, but only 2 bytes free
    EXPECT_FALSE(FrameCodec::encode(buf, h, payload));
}

TEST(FrameCodec, EncodeToBufferTooSmall)
{
    std::array<uint8_t, 4> payload{0x01, 0x02, 0x03, 0x04};
    WireHeader h{.msg_id = 1, .body_size = 4, .reserved = {}};

    std::array<uint8_t, 5> small_buf{}; // Too small for header + payload
    auto written = FrameCodec::encode_to(small_buf, h, payload);
    EXPECT_EQ(written, 0u);
}
