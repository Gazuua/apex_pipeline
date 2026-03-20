// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/wire_header.hpp>

#include <gtest/gtest.h>

using namespace apex::core;

TEST(WireHeader, SizeIs12Bytes)
{
    EXPECT_EQ(WireHeader::SIZE, 12u);
}

TEST(WireHeader, DefaultValues)
{
    WireHeader h;
    EXPECT_EQ(h.version, 2u);
    EXPECT_EQ(h.flags, 0u);
    EXPECT_EQ(h.msg_id, 0u);
    EXPECT_EQ(h.body_size, 0u);
    EXPECT_EQ(h.reserved, 0u);
}

TEST(WireHeader, SerializeAndParse)
{
    WireHeader h;
    h.version = 2;
    h.flags = wire_flags::COMPRESSED;
    h.msg_id = 100042;
    h.body_size = 1024;
    h.reserved = 0;

    auto bytes = h.serialize();
    auto result = WireHeader::parse(bytes);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->version, h.version);
    EXPECT_EQ(result->flags, h.flags);
    EXPECT_EQ(result->msg_id, h.msg_id);
    EXPECT_EQ(result->body_size, h.body_size);
    EXPECT_EQ(result->reserved, h.reserved);
}

TEST(WireHeader, BigEndianByteOrder)
{
    WireHeader h;
    h.flags = 0x12;
    h.msg_id = 0x01020304;
    h.body_size = 0x05060708;

    auto bytes = h.serialize();

    // flags at [1] -- u8 direct
    EXPECT_EQ(bytes[1], 0x12);

    // msg_id at [2..5] big-endian
    EXPECT_EQ(bytes[2], 0x01);
    EXPECT_EQ(bytes[3], 0x02);
    EXPECT_EQ(bytes[4], 0x03);
    EXPECT_EQ(bytes[5], 0x04);

    // body_size at [6..9] big-endian
    EXPECT_EQ(bytes[6], 0x05);
    EXPECT_EQ(bytes[7], 0x06);
    EXPECT_EQ(bytes[8], 0x07);
    EXPECT_EQ(bytes[9], 0x08);
}

TEST(WireHeader, ParseInsufficientData)
{
    std::array<uint8_t, 5> data{};
    auto result = WireHeader::parse(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::InsufficientData);
}

TEST(WireHeader, ParseBodyTooLarge)
{
    WireHeader h;
    h.body_size = WireHeader::MAX_BODY_SIZE + 1;

    // Manually serialize with oversized body_size
    auto bytes = h.serialize();
    // parse should reject it
    auto result = WireHeader::parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::BodyTooLarge);
}

TEST(WireHeader, FrameSize)
{
    WireHeader h;
    h.body_size = 256;
    EXPECT_EQ(h.frame_size(), 268u); // 12 + 256 = 268
}

TEST(WireHeader, ZeroBodySize)
{
    WireHeader h;
    h.msg_id = 100;
    h.body_size = 0;

    auto bytes = h.serialize();
    auto result = WireHeader::parse(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->body_size, 0u);
    EXPECT_EQ(result->msg_id, 100u);
}

TEST(WireHeader, ParseUnsupportedVersion)
{
    WireHeader h;
    auto bytes = h.serialize();
    bytes[0] = 0; // version 0
    auto result = WireHeader::parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::UnsupportedVersion);
}

TEST(WireHeader, ParseV1Rejected)
{
    WireHeader h;
    auto bytes = h.serialize();
    bytes[0] = 1; // old v1 version
    auto result = WireHeader::parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::UnsupportedVersion);
}

TEST(WireHeader, ParseFutureVersion)
{
    WireHeader h;
    auto bytes = h.serialize();
    bytes[0] = 99; // far-future version
    auto result = WireHeader::parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::UnsupportedVersion);
}

TEST(WireHeader, MaxValidBodySize)
{
    WireHeader h;
    h.body_size = WireHeader::MAX_BODY_SIZE;

    auto bytes = h.serialize();
    auto result = WireHeader::parse(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->body_size, WireHeader::MAX_BODY_SIZE);
}

TEST(WireHeader, LargeMsgId)
{
    WireHeader h;
    h.msg_id = 0xDEADBEEF;
    auto bytes = h.serialize();
    auto result = WireHeader::parse(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_id, 0xDEADBEEF);
}
