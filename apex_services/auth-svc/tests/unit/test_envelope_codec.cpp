// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace envelope = apex::shared::protocols::kafka;

TEST(EnvelopeCodecTest, BuildAndParseRoutingHeaderRoundTrip)
{
    envelope::RoutingHeader routing;
    routing.header_version = envelope::RoutingHeader::CURRENT_VERSION;
    routing.flags = envelope::routing_flags::DIRECTION_RESPONSE;
    routing.msg_id = 1001;

    auto bytes = routing.serialize();
    EXPECT_EQ(bytes.size(), envelope::RoutingHeader::SIZE);

    auto parsed = envelope::RoutingHeader::parse(std::span<const uint8_t>(bytes));
    ASSERT_TRUE(parsed.has_value());

    EXPECT_EQ(parsed->header_version, envelope::RoutingHeader::CURRENT_VERSION);
    EXPECT_EQ(parsed->flags, envelope::routing_flags::DIRECTION_RESPONSE);
    EXPECT_EQ(parsed->msg_id, 1001u);
}

TEST(EnvelopeCodecTest, BuildAndParseMetadataRoundTrip)
{
    envelope::MetadataPrefix metadata;
    metadata.meta_version = envelope::MetadataPrefix::CURRENT_VERSION;
    metadata.core_id = 3;
    metadata.corr_id = 12345678;
    metadata.source_id = envelope::source_ids::AUTH;
    metadata.session_id = 987654321;
    metadata.timestamp = 1710000000000ULL;

    auto bytes = metadata.serialize();
    EXPECT_EQ(bytes.size(), envelope::MetadataPrefix::SIZE);

    auto parsed = envelope::MetadataPrefix::parse(std::span<const uint8_t>(bytes));
    ASSERT_TRUE(parsed.has_value());

    EXPECT_EQ(parsed->core_id, 3);
    EXPECT_EQ(parsed->corr_id, 12345678u);
    EXPECT_EQ(parsed->source_id, envelope::source_ids::AUTH);
    EXPECT_EQ(parsed->session_id, 987654321u);
    EXPECT_EQ(parsed->timestamp, 1710000000000ULL);
}

TEST(EnvelopeCodecTest, FullEnvelopeRoundTrip)
{
    envelope::RoutingHeader routing;
    routing.header_version = envelope::RoutingHeader::CURRENT_VERSION;
    routing.flags = envelope::routing_flags::DIRECTION_RESPONSE;
    routing.msg_id = 1001;

    envelope::MetadataPrefix metadata;
    metadata.meta_version = envelope::MetadataPrefix::CURRENT_VERSION;
    metadata.core_id = 3;
    metadata.corr_id = 12345678;
    metadata.source_id = envelope::source_ids::AUTH;
    metadata.session_id = 987654321;
    metadata.timestamp = 1710000000000ULL;

    // Build full envelope: routing(8B) + metadata(40B) + payload
    std::vector<uint8_t> payload_data = {0xDE, 0xAD, 0xBE, 0xEF};

    auto routing_bytes = routing.serialize();
    auto metadata_bytes = metadata.serialize();

    std::vector<uint8_t> full_envelope;
    full_envelope.insert(full_envelope.end(), routing_bytes.begin(), routing_bytes.end());
    full_envelope.insert(full_envelope.end(), metadata_bytes.begin(), metadata_bytes.end());
    full_envelope.insert(full_envelope.end(), payload_data.begin(), payload_data.end());

    EXPECT_EQ(full_envelope.size(), envelope::ENVELOPE_HEADER_SIZE + payload_data.size());

    // Parse back
    auto parsed_routing = envelope::RoutingHeader::parse(std::span<const uint8_t>(full_envelope));
    ASSERT_TRUE(parsed_routing.has_value());
    EXPECT_EQ(parsed_routing->msg_id, 1001u);

    auto parsed_metadata =
        envelope::MetadataPrefix::parse(std::span<const uint8_t>(full_envelope).subspan(envelope::RoutingHeader::SIZE));
    ASSERT_TRUE(parsed_metadata.has_value());
    EXPECT_EQ(parsed_metadata->corr_id, 12345678u);

    // Payload extraction
    auto fbs_payload = std::span<const uint8_t>(full_envelope).subspan(envelope::ENVELOPE_HEADER_SIZE);
    ASSERT_EQ(fbs_payload.size(), 4u);
    EXPECT_EQ(fbs_payload[0], 0xDE);
    EXPECT_EQ(fbs_payload[3], 0xEF);
}

TEST(EnvelopeCodecTest, TooSmallRoutingHeaderFails)
{
    std::vector<uint8_t> small(4);
    auto result = envelope::RoutingHeader::parse(std::span<const uint8_t>(small));
    EXPECT_FALSE(result.has_value());
}

TEST(EnvelopeCodecTest, TooSmallMetadataFails)
{
    std::vector<uint8_t> small(10);
    auto result = envelope::MetadataPrefix::parse(std::span<const uint8_t>(small));
    EXPECT_FALSE(result.has_value());
}

TEST(EnvelopeFlagsTest, ResponseBit)
{
    EXPECT_EQ(envelope::routing_flags::DIRECTION_RESPONSE & 0x0001, 0x0001);
    uint16_t flags = 0;
    flags |= envelope::routing_flags::DIRECTION_RESPONSE;
    EXPECT_TRUE((flags & envelope::routing_flags::DIRECTION_RESPONSE) != 0);
}

TEST(EnvelopeFlagsTest, ErrorBit)
{
    EXPECT_EQ(envelope::routing_flags::ERROR_BIT, 0x0002);
    uint16_t flags = 0;
    flags |= envelope::routing_flags::ERROR_BIT;
    EXPECT_TRUE((flags & envelope::routing_flags::ERROR_BIT) != 0);
}

TEST(EnvelopeFlagsTest, DeliveryModes)
{
    EXPECT_EQ(envelope::routing_flags::DELIVERY_UNICAST, 0x0000);
    EXPECT_EQ(envelope::routing_flags::DELIVERY_CHANNEL, 0x0004);
    EXPECT_EQ(envelope::routing_flags::DELIVERY_BROADCAST, 0x0008);
}

TEST(EnvelopeSourceIdsTest, ServiceIds)
{
    EXPECT_EQ(envelope::source_ids::GATEWAY, 0);
    EXPECT_EQ(envelope::source_ids::AUTH, 1);
    EXPECT_EQ(envelope::source_ids::CHAT, 2);
}
