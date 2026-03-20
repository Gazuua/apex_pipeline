// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/protocols/kafka/envelope_builder.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

#include <apex/core/bump_allocator.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace apex::shared::protocols::kafka;
using apex::core::BumpAllocator;

// ============================================================
// 헬퍼: build_full_envelope()과 EnvelopeBuilder 결과를 비교할 때
// timestamp 필드는 비교 대상에서 제외한다.
// (EnvelopeBuilder.build()는 현재 시각을 auto-stamp하므로
//  두 호출 사이에 값이 달라질 수 있음)
// ============================================================

static constexpr size_t TIMESTAMP_OFFSET = RoutingHeader::SIZE + MetadataPrefix::OFF_TIMESTAMP; // 8 + 32 = 40

/// timestamp 8바이트를 0으로 마스킹한 뒤 비교
static bool bytes_eq_ignore_timestamp(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    if (a.size() != b.size())
        return false;

    std::vector<uint8_t> ca(a), cb(b);
    if (ca.size() >= TIMESTAMP_OFFSET + 8)
    {
        std::fill(ca.begin() + TIMESTAMP_OFFSET, ca.begin() + TIMESTAMP_OFFSET + 8, 0u);
        std::fill(cb.begin() + TIMESTAMP_OFFSET, cb.begin() + TIMESTAMP_OFFSET + 8, 0u);
    }
    return ca == cb;
}

// ============================================================
// 체인 인터페이스 테스트
// ============================================================

TEST(EnvelopeBuilder, ChainableReturnsThis)
{
    // 메서드 체인이 동일한 객체를 반환하는지 검증
    std::vector<uint8_t> data{0x01, 0x02};
    EnvelopeBuilder builder;
    EnvelopeBuilder& ref = builder.routing(42u)
                               .metadata(1u, 99ull, source_ids::AUTH, 10ull, 20ull)
                               .reply_topic("test.topic")
                               .payload(data);
    EXPECT_EQ(&ref, &builder);
}

// ============================================================
// build()이 build_full_envelope()과 동일한 레이아웃을 생성하는지 검증
// ============================================================

TEST(EnvelopeBuilder, BuildMatchesBuildFullEnvelope_NoReplyTopic)
{
    std::vector<uint8_t> payload{0xAA, 0xBB, 0xCC};

    // 기준: build_full_envelope()
    RoutingHeader rh;
    rh.msg_id = 100u;
    rh.flags = routing_flags::PRIORITY_HIGH;
    MetadataPrefix mp;
    mp.core_id = 3u;
    mp.corr_id = 0x1122334455667788ull;
    mp.source_id = source_ids::GATEWAY;
    mp.session_id = 0xDEADull;
    mp.user_id = 42ull;
    // timestamp는 build_full_envelope()도 0으로 유지 (기본값)

    auto expected = build_full_envelope(rh, mp, "", payload);

    // EnvelopeBuilder
    auto actual = EnvelopeBuilder{}
                      .routing(100u, routing_flags::PRIORITY_HIGH)
                      .metadata(3u, 0x1122334455667788ull, source_ids::GATEWAY, 0xDEADull, 42ull)
                      .payload(payload)
                      .build();

    // 사이즈 일치
    ASSERT_EQ(expected.size(), actual.size());
    // timestamp 제외 나머지 바이트 일치
    EXPECT_TRUE(bytes_eq_ignore_timestamp(expected, actual));

    // HAS_REPLY_TOPIC 플래그 없어야 함
    auto parsed = RoutingHeader::parse(actual);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->flags & routing_flags::HAS_REPLY_TOPIC, 0u);
}

TEST(EnvelopeBuilder, BuildMatchesBuildFullEnvelope_WithReplyTopic)
{
    std::vector<uint8_t> payload{0x01, 0x02, 0x03};
    std::string topic = "auth.responses";

    RoutingHeader rh;
    rh.msg_id = 200u;
    MetadataPrefix mp;
    mp.source_id = source_ids::AUTH;
    mp.session_id = 0xCAFEBABEull;
    mp.user_id = 99ull;

    auto expected = build_full_envelope(rh, mp, topic, payload);

    auto actual = EnvelopeBuilder{}
                      .routing(200u)
                      .metadata(0u, 0ull, source_ids::AUTH, 0xCAFEBABEull, 99ull)
                      .reply_topic(topic)
                      .payload(payload)
                      .build();

    ASSERT_EQ(expected.size(), actual.size());
    EXPECT_TRUE(bytes_eq_ignore_timestamp(expected, actual));

    // HAS_REPLY_TOPIC 플래그 설정 확인
    auto parsed = RoutingHeader::parse(actual);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_NE(parsed->flags & routing_flags::HAS_REPLY_TOPIC, 0u);

    // reply_topic 추출 확인
    auto extracted = extract_reply_topic(parsed->flags, actual);
    EXPECT_EQ(extracted, topic);
}

// ============================================================
// 기본값 동작 검증
// ============================================================

TEST(EnvelopeBuilder, DefaultFlagsIsZero)
{
    // flags 기본값 0: DIRECTION_REQUEST, DELIVERY_UNICAST
    auto actual = EnvelopeBuilder{}
                      .routing(0u) // flags 생략 → 기본값 0
                      .metadata(0u, 0ull, 0u, 0ull, 0ull)
                      .build();

    auto parsed = RoutingHeader::parse(actual);
    ASSERT_TRUE(parsed.has_value());
    // HAS_REPLY_TOPIC 포함 어떠한 플래그도 설정되지 않아야 함
    EXPECT_EQ(parsed->flags, 0u);
}

TEST(EnvelopeBuilder, EmptyReplyTopicOmitsSection)
{
    // reply_topic()을 빈 문자열로 설정하면 ReplyTopic 섹션 생략
    std::vector<uint8_t> payload{0xFF};
    auto actual = EnvelopeBuilder{}
                      .routing(1u)
                      .metadata(0u, 0ull, 0u, 0ull, 0ull)
                      .reply_topic("") // 명시적 빈 문자열
                      .payload(payload)
                      .build();

    // 크기: RoutingHeader(8) + MetadataPrefix(40) + payload(1) = 49
    EXPECT_EQ(actual.size(), ENVELOPE_HEADER_SIZE + 1u);

    auto parsed = RoutingHeader::parse(actual);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->flags & routing_flags::HAS_REPLY_TOPIC, 0u);
}

TEST(EnvelopeBuilder, NoPayloadBuild)
{
    // payload 미설정 시 ENVELOPE_HEADER_SIZE만큼
    auto actual = EnvelopeBuilder{}.routing(5u).metadata(0u, 0ull, 0u, 0ull, 0ull).build();

    EXPECT_EQ(actual.size(), ENVELOPE_HEADER_SIZE);
}

// ============================================================
// build_into(BumpAllocator&) 핫패스 검증
// ============================================================

TEST(EnvelopeBuilder, BuildIntoBumpAllocator_SameBytesAsVectorBuild)
{
    std::vector<uint8_t> payload{0x11, 0x22, 0x33, 0x44};

    // BumpAllocator에 충분한 공간 확보
    BumpAllocator alloc(4096);

    auto span_result = EnvelopeBuilder{}
                           .routing(77u, routing_flags::DELIVERY_UNICAST)
                           .metadata(2u, 0xABCDull, source_ids::CHAT, 0x1234ull, 55ull)
                           .reply_topic("chat.out")
                           .payload(payload)
                           .build_into(alloc);

    ASSERT_FALSE(span_result.empty());

    // 동일한 빌더로 vector build() 수행 (비교 기준)
    auto vec_result = EnvelopeBuilder{}
                          .routing(77u, routing_flags::DELIVERY_UNICAST)
                          .metadata(2u, 0xABCDull, source_ids::CHAT, 0x1234ull, 55ull)
                          .reply_topic("chat.out")
                          .payload(payload)
                          .build();

    ASSERT_EQ(span_result.size(), vec_result.size());

    std::vector<uint8_t> span_vec(span_result.begin(), span_result.end());
    EXPECT_TRUE(bytes_eq_ignore_timestamp(span_vec, vec_result));
}

TEST(EnvelopeBuilder, BuildIntoAllocatesFromBump)
{
    BumpAllocator alloc(1024);
    EXPECT_EQ(alloc.used_bytes(), 0u);

    std::vector<uint8_t> payload{0xDE, 0xAD};
    auto span_result =
        EnvelopeBuilder{}.routing(10u).metadata(0u, 0ull, 0u, 0ull, 0ull).payload(payload).build_into(alloc);

    // 예상 크기: ENVELOPE_HEADER_SIZE + 2 = 50
    const size_t expected_size = ENVELOPE_HEADER_SIZE + 2;
    ASSERT_EQ(span_result.size(), expected_size);
    // BumpAllocator에서 해당 크기만큼 사용됨
    EXPECT_EQ(alloc.used_bytes(), expected_size);
    // 반환된 포인터는 allocator가 소유
    EXPECT_TRUE(alloc.owns(span_result.data()));
}

TEST(EnvelopeBuilder, BuildIntoMultipleAllocationsPossible)
{
    BumpAllocator alloc(4096);

    std::vector<uint8_t> p1{0x01};
    std::vector<uint8_t> p2{0x02, 0x03};

    auto s1 = EnvelopeBuilder{}.routing(1u).metadata(0u, 0ull, 0u, 0ull, 0ull).payload(p1).build_into(alloc);
    auto s2 = EnvelopeBuilder{}.routing(2u).metadata(0u, 0ull, 0u, 0ull, 0ull).payload(p2).build_into(alloc);

    // 두 번 모두 성공
    ASSERT_FALSE(s1.empty());
    ASSERT_FALSE(s2.empty());
    // 두 span이 겹치지 않아야 함
    EXPECT_NE(s1.data(), s2.data());
}

TEST(EnvelopeBuilder, BuildIntoReturnsEmptyOnOverflow)
{
    // 용량 부족한 BumpAllocator
    BumpAllocator alloc(8); // ENVELOPE_HEADER_SIZE(48)보다 훨씬 작음

    std::vector<uint8_t> payload{0x01};
    auto span_result =
        EnvelopeBuilder{}.routing(1u).metadata(0u, 0ull, 0u, 0ull, 0ull).payload(payload).build_into(alloc);

    EXPECT_TRUE(span_result.empty());
}

// ============================================================
// reply_topic 포함/미포함 구조 검증
// ============================================================

TEST(EnvelopeBuilder, ReplyTopicSection_PayloadOffset)
{
    std::string topic = "test.topic";
    std::vector<uint8_t> payload{0xBE, 0xEF};

    auto actual =
        EnvelopeBuilder{}.routing(1u).metadata(0u, 0ull, 0u, 0ull, 0ull).reply_topic(topic).payload(payload).build();

    auto parsed_rh = RoutingHeader::parse(actual);
    ASSERT_TRUE(parsed_rh.has_value());

    // payload offset 검증
    auto offset = envelope_payload_offset(parsed_rh->flags, actual);
    size_t expected_offset = ENVELOPE_HEADER_SIZE + sizeof(uint16_t) + topic.size();
    EXPECT_EQ(offset, expected_offset);

    // payload 바이트 검증
    ASSERT_GE(actual.size(), offset + payload.size());
    EXPECT_EQ(actual[offset], 0xBEu);
    EXPECT_EQ(actual[offset + 1], 0xEFu);
}

TEST(EnvelopeBuilder, HAS_REPLY_TOPIC_AutoClearedFromFlags)
{
    // 호출자가 flags에 HAS_REPLY_TOPIC을 수동 설정했어도
    // reply_topic이 비어있으면 build 시 자동 해제
    auto actual = EnvelopeBuilder{}
                      .routing(1u, routing_flags::HAS_REPLY_TOPIC) // 수동 설정
                      .metadata(0u, 0ull, 0u, 0ull, 0ull)
                      .reply_topic("") // 비어있음 → HAS_REPLY_TOPIC 해제돼야 함
                      .build();

    auto parsed = RoutingHeader::parse(actual);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->flags & routing_flags::HAS_REPLY_TOPIC, 0u);
}

TEST(EnvelopeBuilder, HAS_REPLY_TOPIC_AutoSetWhenTopicGiven)
{
    // flags=0으로 설정해도 reply_topic이 있으면 HAS_REPLY_TOPIC 자동 설정
    auto actual = EnvelopeBuilder{}
                      .routing(1u, 0u) // flags=0
                      .metadata(0u, 0ull, 0u, 0ull, 0ull)
                      .reply_topic("svc.replies")
                      .build();

    auto parsed = RoutingHeader::parse(actual);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_NE(parsed->flags & routing_flags::HAS_REPLY_TOPIC, 0u);
}

// ============================================================
// 기타 플래그 값이 build 후에도 유지되는지 검증
// ============================================================

TEST(EnvelopeBuilder, OtherFlagsPreservedInBuild)
{
    const uint16_t test_flags =
        routing_flags::PRIORITY_HIGH | routing_flags::ENCRYPTED | routing_flags::DELIVERY_CHANNEL;

    auto actual = EnvelopeBuilder{}.routing(99u, test_flags).metadata(0u, 0ull, 0u, 0ull, 0ull).build();

    auto parsed = RoutingHeader::parse(actual);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->msg_id, 99u);
    // HAS_REPLY_TOPIC을 제외한 나머지 플래그 보존
    EXPECT_EQ(parsed->flags & ~routing_flags::HAS_REPLY_TOPIC, test_flags & ~routing_flags::HAS_REPLY_TOPIC);
}
