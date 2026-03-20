// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

#include <gtest/gtest.h>

using namespace apex::shared::protocols::kafka;

// === RoutingHeader Tests ===

TEST(RoutingHeader, SizeIs8Bytes)
{
    EXPECT_EQ(RoutingHeader::SIZE, 8u);
}

TEST(RoutingHeader, DefaultValues)
{
    RoutingHeader h;
    EXPECT_EQ(h.header_version, RoutingHeader::CURRENT_VERSION);
    EXPECT_EQ(h.flags, 0u);
    EXPECT_EQ(h.msg_id, 0u);
}

TEST(RoutingHeader, SerializeAndParse)
{
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

TEST(RoutingHeader, BigEndianByteOrder)
{
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

TEST(RoutingHeader, ParseInsufficientData)
{
    std::array<uint8_t, 4> data{};
    auto result = RoutingHeader::parse(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EnvelopeError::InsufficientData);
}

TEST(RoutingHeader, ParseUnsupportedVersion)
{
    RoutingHeader h;
    auto bytes = h.serialize();
    bytes[0] = 0x00;
    bytes[1] = 0x99; // version 153
    auto result = RoutingHeader::parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EnvelopeError::UnsupportedVersion);
}

TEST(RoutingHeader, DeliveryFlags)
{
    RoutingHeader h;
    h.flags = routing_flags::DELIVERY_CHANNEL;
    EXPECT_EQ(h.flags & routing_flags::DELIVERY_MASK, routing_flags::DELIVERY_CHANNEL);

    h.flags = routing_flags::DELIVERY_BROADCAST;
    EXPECT_EQ(h.flags & routing_flags::DELIVERY_MASK, routing_flags::DELIVERY_BROADCAST);
}

// === MetadataPrefix Tests ===

TEST(MetadataPrefix, SizeIs40Bytes)
{
    EXPECT_EQ(MetadataPrefix::SIZE, 40u);
}

TEST(MetadataPrefix, EnvelopeHeaderSizeIs48)
{
    EXPECT_EQ(ENVELOPE_HEADER_SIZE, 48u);
}

TEST(MetadataPrefix, DefaultValues)
{
    MetadataPrefix m;
    EXPECT_EQ(m.meta_version, MetadataPrefix::CURRENT_VERSION);
    EXPECT_EQ(m.core_id, 0u);
    EXPECT_EQ(m.corr_id, 0u);
    EXPECT_EQ(m.source_id, 0u);
    EXPECT_EQ(m.session_id, 0u);
    EXPECT_EQ(m.user_id, 0u);
    EXPECT_EQ(m.timestamp, 0u);
}

TEST(MetadataPrefix, SerializeAndParse)
{
    MetadataPrefix m;
    m.core_id = 7;
    m.corr_id = 0x0123456789ABCDEFull;
    m.source_id = source_ids::AUTH;
    m.session_id = 0xFEDCBA9876543210ull;
    m.user_id = 42;
    m.timestamp = 1710547200000ull;

    auto bytes = m.serialize();
    auto result = MetadataPrefix::parse(bytes);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->meta_version, MetadataPrefix::CURRENT_VERSION);
    EXPECT_EQ(result->core_id, 7u);
    EXPECT_EQ(result->corr_id, 0x0123456789ABCDEFull);
    EXPECT_EQ(result->source_id, source_ids::AUTH);
    EXPECT_EQ(result->session_id, 0xFEDCBA9876543210ull);
    EXPECT_EQ(result->user_id, 42u);
    EXPECT_EQ(result->timestamp, 1710547200000ull);
}

TEST(MetadataPrefix, ParseInsufficientData)
{
    std::array<uint8_t, 16> data{};
    auto result = MetadataPrefix::parse(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EnvelopeError::InsufficientData);
}

TEST(MetadataPrefix, ParseUnsupportedVersion)
{
    MetadataPrefix m;
    auto bytes = m.serialize();
    // 버전 바이트를 99로 변경
    bytes[0] = 0x00;
    bytes[1] = 0x00;
    bytes[2] = 0x00;
    bytes[3] = 0x63;
    auto result = MetadataPrefix::parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EnvelopeError::UnsupportedVersion);
}

TEST(MetadataPrefix, SourceIds)
{
    EXPECT_EQ(source_ids::GATEWAY, 0u);
    EXPECT_EQ(source_ids::AUTH, 1u);
    EXPECT_EQ(source_ids::CHAT, 2u);
}

// === ReplyTopicHeader Tests ===

TEST(ReplyTopicHeader, SerializeEmpty)
{
    auto bytes = ReplyTopicHeader::serialize("");
    EXPECT_TRUE(bytes.empty());
}

TEST(ReplyTopicHeader, SerializeAndParse)
{
    std::string topic = "auth.responses";
    auto bytes = ReplyTopicHeader::serialize(topic);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.size(), sizeof(uint16_t) + topic.size());

    auto result = ReplyTopicHeader::parse(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, topic);
    EXPECT_EQ(result->second, bytes.size());
}

TEST(ReplyTopicHeader, ParseInsufficientData_TooShort)
{
    std::array<uint8_t, 1> data{};
    auto result = ReplyTopicHeader::parse(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EnvelopeError::InsufficientData);
}

TEST(ReplyTopicHeader, ParseInsufficientData_TopicTruncated)
{
    // 헤더에 길이 10을 기록하지만 실제 데이터는 3바이트만 제공
    std::vector<uint8_t> data(sizeof(uint16_t) + 3);
    // big-endian으로 길이 10 기록
    data[0] = 0x00;
    data[1] = 0x0A; // length = 10
    // 나머지 3바이트만 있으므로 파싱 실패해야 함
    auto result = ReplyTopicHeader::parse(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), EnvelopeError::InsufficientData);
}

TEST(ReplyTopicHeader, SerializeOverflowReturnsEmpty)
{
    // uint16_t max = 65535. 그보다 큰 topic은 빈 벡터 반환해야 함
    // 실제로 65536바이트 문자열을 만들어서 테스트
    std::string oversized_topic(65536, 'A');
    auto bytes = ReplyTopicHeader::serialize(oversized_topic);
    EXPECT_TRUE(bytes.empty());
}

TEST(ReplyTopicHeader, SerializeMaxValidLength)
{
    // uint16_t max = 65535 — 이 길이는 유효해야 함
    std::string max_topic(65535, 'B');
    auto bytes = ReplyTopicHeader::serialize(max_topic);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.size(), sizeof(uint16_t) + 65535);
}

// === extract_reply_topic Tests ===

TEST(ExtractReplyTopic, NoFlag)
{
    // HAS_REPLY_TOPIC 플래그 없으면 빈 문자열 반환
    auto topic = extract_reply_topic(0, {});
    EXPECT_TRUE(topic.empty());
}

TEST(ExtractReplyTopic, WithReplyTopic)
{
    RoutingHeader rh;
    rh.msg_id = 100;
    MetadataPrefix mp;
    mp.core_id = 1;

    std::string reply_topic = "chat.responses";
    auto envelope = build_full_envelope(rh, mp, reply_topic, {});

    auto extracted = extract_reply_topic(routing_flags::HAS_REPLY_TOPIC, envelope);
    EXPECT_EQ(extracted, reply_topic);
}

TEST(ExtractReplyTopic, InsufficientDataReturnsEmpty)
{
    // HAS_REPLY_TOPIC 플래그 있지만 데이터가 부족하면 빈 문자열
    std::vector<uint8_t> short_data(ENVELOPE_HEADER_SIZE);
    auto topic = extract_reply_topic(routing_flags::HAS_REPLY_TOPIC, short_data);
    EXPECT_TRUE(topic.empty());
}

// === build_full_envelope Tests ===

TEST(BuildFullEnvelope, WithoutReplyTopic)
{
    RoutingHeader rh;
    rh.msg_id = 42;
    rh.flags = routing_flags::HAS_REPLY_TOPIC; // 일부러 설정해도 빈 topic이면 해제되어야 함

    MetadataPrefix mp;
    std::vector<uint8_t> payload{0x01, 0x02, 0x03};

    auto envelope = build_full_envelope(rh, mp, "", payload);

    // HAS_REPLY_TOPIC 플래그 해제 확인
    auto parsed_rh = RoutingHeader::parse(envelope);
    ASSERT_TRUE(parsed_rh.has_value());
    EXPECT_EQ(parsed_rh->flags & routing_flags::HAS_REPLY_TOPIC, 0u);

    // payload offset은 ENVELOPE_HEADER_SIZE
    auto offset = envelope_payload_offset(parsed_rh->flags, envelope);
    EXPECT_EQ(offset, ENVELOPE_HEADER_SIZE);
}

TEST(BuildFullEnvelope, WithReplyTopic)
{
    RoutingHeader rh;
    rh.msg_id = 42;

    MetadataPrefix mp;
    std::string reply_topic = "test.topic";
    std::vector<uint8_t> payload{0xAA, 0xBB};

    auto envelope = build_full_envelope(rh, mp, reply_topic, payload);

    // HAS_REPLY_TOPIC 플래그 자동 설정 확인
    auto parsed_rh = RoutingHeader::parse(envelope);
    ASSERT_TRUE(parsed_rh.has_value());
    EXPECT_NE(parsed_rh->flags & routing_flags::HAS_REPLY_TOPIC, 0u);

    // reply topic 추출 확인
    auto extracted = extract_reply_topic(parsed_rh->flags, envelope);
    EXPECT_EQ(extracted, reply_topic);

    // payload offset 확인
    auto offset = envelope_payload_offset(parsed_rh->flags, envelope);
    size_t expected_offset = ENVELOPE_HEADER_SIZE + sizeof(uint16_t) + reply_topic.size();
    EXPECT_EQ(offset, expected_offset);
}

// === envelope_payload_offset 엣지케이스 Tests ===

TEST(EnvelopePayloadOffset, NoReplyTopicFlag)
{
    // HAS_REPLY_TOPIC 플래그 없으면 ENVELOPE_HEADER_SIZE 반환
    std::vector<uint8_t> data(ENVELOPE_HEADER_SIZE + 10, 0);
    auto offset = envelope_payload_offset(0, data);
    EXPECT_EQ(offset, ENVELOPE_HEADER_SIZE);
}

TEST(EnvelopePayloadOffset, HasReplyTopicFlagButParseFails)
{
    // HAS_REPLY_TOPIC 플래그 있지만 reply_topic 데이터가 부족하여 parse 실패 시
    // offset은 ENVELOPE_HEADER_SIZE 그대로 반환 (fallback)
    std::vector<uint8_t> data(ENVELOPE_HEADER_SIZE + 1, 0); // uint16_t 파싱 불가 (1바이트만)
    auto offset = envelope_payload_offset(routing_flags::HAS_REPLY_TOPIC, data);
    EXPECT_EQ(offset, ENVELOPE_HEADER_SIZE);
}

TEST(EnvelopePayloadOffset, HasReplyTopicFlagButDataTooShort)
{
    // HAS_REPLY_TOPIC 플래그 있지만 data가 ENVELOPE_HEADER_SIZE + sizeof(uint16_t) 미만
    std::vector<uint8_t> data(ENVELOPE_HEADER_SIZE, 0); // 딱 헤더 크기만
    auto offset = envelope_payload_offset(routing_flags::HAS_REPLY_TOPIC, data);
    EXPECT_EQ(offset, ENVELOPE_HEADER_SIZE);
}

TEST(EnvelopePayloadOffset, HasReplyTopicFlagTruncatedTopic)
{
    // HAS_REPLY_TOPIC 플래그, uint16_t 파싱 가능하지만 topic 데이터 부족
    std::vector<uint8_t> data(ENVELOPE_HEADER_SIZE + sizeof(uint16_t) + 2, 0);
    // big-endian 길이 = 10 기록 (실제 데이터는 2바이트만)
    data[ENVELOPE_HEADER_SIZE] = 0x00;
    data[ENVELOPE_HEADER_SIZE + 1] = 0x0A; // length = 10
    auto offset = envelope_payload_offset(routing_flags::HAS_REPLY_TOPIC, data);
    // parse 실패하므로 ENVELOPE_HEADER_SIZE 반환
    EXPECT_EQ(offset, ENVELOPE_HEADER_SIZE);
}
