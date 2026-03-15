#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

#include <gtest/gtest.h>

using namespace apex::shared::protocols::kafka;

// === RoutingHeader Tests ===

TEST(RoutingHeader, SizeIs8Bytes) {
    EXPECT_EQ(RoutingHeader::SIZE, 8u);
}

TEST(RoutingHeader, DefaultValues) {
    RoutingHeader h;
    EXPECT_EQ(h.header_version, RoutingHeader::CURRENT_VERSION);
    EXPECT_EQ(h.flags, 0u);
    EXPECT_EQ(h.msg_id, 0u);
}

TEST(RoutingHeader, SerializeAndParse) {
    RoutingHeader h;
    h.flags = routing_flags::DIRECTION_RESPONSE | routing_flags::PRIORITY_HIGH;
    h.msg_id = 0xDEADBEEF;

    auto bytes = h.serialize();
    auto result = RoutingHeader::parse(bytes);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->header_version, RoutingHeader::CURRENT_VERSION);
    EXPECT_EQ(result->flags, h.flags);
    EXPECT_EQ(result->msg_id, h.msg_id);
}

TEST(RoutingHeader, BigEndianByteOrder) {
    RoutingHeader h;
    h.header_version = 1;
    h.flags = 0x1234;
    h.msg_id = 0xAABBCCDD;

    auto bytes = h.serialize();

    // header_version at [0..1] big-endian
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x01);

    // flags at [2..3] big-endian
    EXPECT_EQ(bytes[2], 0x12);
    EXPECT_EQ(bytes[3], 0x34);

    // msg_id at [4..7] big-endian
    EXPECT_EQ(bytes[4], 0xAA);
    EXPECT_EQ(bytes[5], 0xBB);
    EXPECT_EQ(bytes[6], 0xCC);
    EXPECT_EQ(bytes[7], 0xDD);
}

TEST(RoutingHeader, ParseInsufficientData) {
    std::array<uint8_t, 4> data{};
    auto result = RoutingHeader::parse(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EnvelopeError::InsufficientData);
}

TEST(RoutingHeader, ParseUnsupportedVersion) {
    RoutingHeader h;
    auto bytes = h.serialize();
    bytes[0] = 0x00;
    bytes[1] = 0x99;  // version 153
    auto result = RoutingHeader::parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EnvelopeError::UnsupportedVersion);
}

TEST(RoutingHeader, DeliveryFlags) {
    RoutingHeader h;
    h.flags = routing_flags::DELIVERY_CHANNEL;
    EXPECT_EQ(h.flags & routing_flags::DELIVERY_MASK,
              routing_flags::DELIVERY_CHANNEL);

    h.flags = routing_flags::DELIVERY_BROADCAST;
    EXPECT_EQ(h.flags & routing_flags::DELIVERY_MASK,
              routing_flags::DELIVERY_BROADCAST);
}

// === MetadataPrefix Tests ===

TEST(MetadataPrefix, SizeIs32Bytes) {
    EXPECT_EQ(MetadataPrefix::SIZE, 32u);
}

TEST(MetadataPrefix, EnvelopeHeaderSizeIs40) {
    EXPECT_EQ(ENVELOPE_HEADER_SIZE, 40u);
}

TEST(MetadataPrefix, DefaultValues) {
    MetadataPrefix m;
    EXPECT_EQ(m.meta_version, MetadataPrefix::CURRENT_VERSION);
    EXPECT_EQ(m.core_id, 0u);
    EXPECT_EQ(m.corr_id, 0u);
    EXPECT_EQ(m.source_id, 0u);
    EXPECT_EQ(m.session_id, 0u);
    EXPECT_EQ(m.timestamp, 0u);
}

TEST(MetadataPrefix, SerializeAndParse) {
    MetadataPrefix m;
    m.core_id = 7;
    m.corr_id = 0x0123456789ABCDEFull;
    m.source_id = source_ids::AUTH;
    m.session_id = 0xFEDCBA9876543210ull;
    m.timestamp = 1710547200000ull;

    auto bytes = m.serialize();
    auto result = MetadataPrefix::parse(bytes);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->meta_version, MetadataPrefix::CURRENT_VERSION);
    EXPECT_EQ(result->core_id, 7u);
    EXPECT_EQ(result->corr_id, 0x0123456789ABCDEFull);
    EXPECT_EQ(result->source_id, source_ids::AUTH);
    EXPECT_EQ(result->session_id, 0xFEDCBA9876543210ull);
    EXPECT_EQ(result->timestamp, 1710547200000ull);
}

TEST(MetadataPrefix, ParseInsufficientData) {
    std::array<uint8_t, 16> data{};
    auto result = MetadataPrefix::parse(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EnvelopeError::InsufficientData);
}

TEST(MetadataPrefix, ParseUnsupportedVersion) {
    MetadataPrefix m;
    auto bytes = m.serialize();
    // 버전 바이트를 99로 변경
    bytes[0] = 0x00; bytes[1] = 0x00; bytes[2] = 0x00; bytes[3] = 0x63;
    auto result = MetadataPrefix::parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EnvelopeError::UnsupportedVersion);
}

TEST(MetadataPrefix, SourceIds) {
    EXPECT_EQ(source_ids::GATEWAY, 0u);
    EXPECT_EQ(source_ids::AUTH, 1u);
    EXPECT_EQ(source_ids::CHAT, 2u);
}
