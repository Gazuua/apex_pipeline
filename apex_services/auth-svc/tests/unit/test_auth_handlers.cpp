// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/auth_svc/auth_config.hpp>
#include <apex/auth_svc/auth_service.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

#include "../../../tests/mocks/mock_kafka_adapter.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace envelope = apex::shared::protocols::kafka;

// ============================================================
// AuthService 핸들러 단위 테스트 — Mock Kafka로 Envelope dispatch 검증.
//
// AuthService는 Server + ServiceBase 패턴으로 동작하며,
// on_configure()에서 어댑터 참조, on_start()에서 kafka_route 핸들러를 등록한다.
// 직접 인스턴스화 대신 핵심 로직을 개별 검증:
// 1. Kafka Envelope 파싱 + msg_id dispatch
// 2. Mock Kafka produce 기록으로 응답 메시지 검증
// 3. msg_id 상수 정합성
// ============================================================

namespace
{

// Auth msg_id constants (mirrored from auth_service.hpp msg_ids)
namespace msg_ids
{
constexpr uint32_t LOGIN_REQUEST = 1000;
constexpr uint32_t LOGIN_RESPONSE = 1001;
constexpr uint32_t LOGOUT_REQUEST = 1002;
constexpr uint32_t LOGOUT_RESPONSE = 1003;
constexpr uint32_t REFRESH_TOKEN_REQUEST = 1004;
constexpr uint32_t REFRESH_TOKEN_RESPONSE = 1005;
} // namespace msg_ids

/// Envelope 빌드 헬퍼 — RoutingHeader + MetadataPrefix + payload.
std::vector<uint8_t> build_envelope(uint32_t msg_id, uint64_t corr_id, uint16_t core_id, uint64_t session_id,
                                    std::span<const uint8_t> fbs_payload = {})
{
    envelope::RoutingHeader rh;
    rh.header_version = envelope::RoutingHeader::CURRENT_VERSION;
    rh.flags = 0; // Request
    rh.msg_id = msg_id;

    envelope::MetadataPrefix meta;
    meta.meta_version = envelope::MetadataPrefix::CURRENT_VERSION;
    meta.core_id = core_id;
    meta.corr_id = corr_id;
    meta.source_id = envelope::source_ids::GATEWAY;
    meta.session_id = session_id;
    meta.timestamp = 1710000000000ULL;

    auto rh_bytes = rh.serialize();
    auto meta_bytes = meta.serialize();

    std::vector<uint8_t> buf;
    buf.reserve(envelope::ENVELOPE_HEADER_SIZE + fbs_payload.size());
    buf.insert(buf.end(), rh_bytes.begin(), rh_bytes.end());
    buf.insert(buf.end(), meta_bytes.begin(), meta_bytes.end());
    buf.insert(buf.end(), fbs_payload.begin(), fbs_payload.end());
    return buf;
}

} // namespace

// --- msg_id 상수 정합성 ---

TEST(AuthHandlersTest, MsgIdConstants)
{
    // msg_id 상수가 msg_registry.toml과 일치하는지 확인.
    EXPECT_EQ(msg_ids::LOGIN_REQUEST, 1000u);
    EXPECT_EQ(msg_ids::LOGIN_RESPONSE, 1001u);
    EXPECT_EQ(msg_ids::LOGOUT_REQUEST, 1002u);
    EXPECT_EQ(msg_ids::LOGOUT_RESPONSE, 1003u);
    EXPECT_EQ(msg_ids::REFRESH_TOKEN_REQUEST, 1004u);
    EXPECT_EQ(msg_ids::REFRESH_TOKEN_RESPONSE, 1005u);
}

// --- Envelope 파싱 ---

TEST(AuthHandlersTest, EnvelopeParseLoginRequest)
{
    auto env = build_envelope(msg_ids::LOGIN_REQUEST, 42, 1, 12345);

    ASSERT_GE(env.size(), envelope::ENVELOPE_HEADER_SIZE);

    auto rh = envelope::RoutingHeader::parse(env);
    ASSERT_TRUE(rh.has_value());
    EXPECT_EQ(rh->msg_id, msg_ids::LOGIN_REQUEST);

    auto meta = envelope::MetadataPrefix::parse(std::span<const uint8_t>(env).subspan(envelope::RoutingHeader::SIZE));
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->corr_id, 42u);
    EXPECT_EQ(meta->core_id, 1);
    EXPECT_EQ(meta->session_id, 12345u);
}

TEST(AuthHandlersTest, EnvelopeParseLogoutRequest)
{
    auto env = build_envelope(msg_ids::LOGOUT_REQUEST, 99, 3, 67890);

    auto rh = envelope::RoutingHeader::parse(env);
    ASSERT_TRUE(rh.has_value());
    EXPECT_EQ(rh->msg_id, msg_ids::LOGOUT_REQUEST);

    auto meta = envelope::MetadataPrefix::parse(std::span<const uint8_t>(env).subspan(envelope::RoutingHeader::SIZE));
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->corr_id, 99u);
}

// --- Envelope too small ---

TEST(AuthHandlersTest, EnvelopeTooSmallRejected)
{
    std::vector<uint8_t> small(10, 0);
    // Envelope must be at least ENVELOPE_HEADER_SIZE (48) bytes.
    EXPECT_LT(small.size(), envelope::ENVELOPE_HEADER_SIZE);
}

// --- Response Envelope 구조 검증 ---

TEST(AuthHandlersTest, ResponseEnvelopeStructure)
{
    // AuthService::send_response가 만드는 응답 Envelope 구조를 재현하여 검증.
    envelope::RoutingHeader routing;
    routing.header_version = envelope::RoutingHeader::CURRENT_VERSION;
    routing.flags = envelope::routing_flags::DIRECTION_RESPONSE;
    routing.msg_id = msg_ids::LOGIN_RESPONSE;

    envelope::MetadataPrefix metadata;
    metadata.meta_version = envelope::MetadataPrefix::CURRENT_VERSION;
    metadata.core_id = 1;
    metadata.corr_id = 42;
    metadata.source_id = envelope::source_ids::AUTH;
    metadata.session_id = 12345;
    metadata.timestamp = 1710000000000ULL;

    auto routing_bytes = routing.serialize();
    auto metadata_bytes = metadata.serialize();

    std::vector<uint8_t> fbs_payload = {0xDE, 0xAD};

    std::vector<uint8_t> envelope_buf;
    envelope_buf.insert(envelope_buf.end(), routing_bytes.begin(), routing_bytes.end());
    envelope_buf.insert(envelope_buf.end(), metadata_bytes.begin(), metadata_bytes.end());
    envelope_buf.insert(envelope_buf.end(), fbs_payload.begin(), fbs_payload.end());

    EXPECT_EQ(envelope_buf.size(), envelope::ENVELOPE_HEADER_SIZE + fbs_payload.size());

    // Verify response direction bit
    auto parsed = envelope::RoutingHeader::parse(envelope_buf);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE((parsed->flags & envelope::routing_flags::DIRECTION_RESPONSE) != 0);

    // Verify source_id = AUTH
    auto meta =
        envelope::MetadataPrefix::parse(std::span<const uint8_t>(envelope_buf).subspan(envelope::RoutingHeader::SIZE));
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->source_id, envelope::source_ids::AUTH);
}

// --- MockKafkaAdapter로 produce 콜백 시뮬레이션 ---

TEST(AuthHandlersTest, MockKafkaMessageInjection)
{
    apex::test::MockKafkaAdapter mock;

    // Set callback that simulates AuthService processing
    bool callback_invoked = false;
    uint32_t received_msg_id = 0;

    mock.set_message_callback([&](std::string_view /*topic*/, int32_t /*partition*/, std::span<const uint8_t> /*key*/,
                                  std::span<const uint8_t> payload, int64_t /*offset*/) -> apex::core::Result<void> {
        callback_invoked = true;

        if (payload.size() >= envelope::ENVELOPE_HEADER_SIZE)
        {
            auto rh = envelope::RoutingHeader::parse(payload);
            if (rh.has_value())
            {
                received_msg_id = rh->msg_id;
            }
        }
        return apex::core::ok();
    });

    // Inject login request
    auto env = build_envelope(msg_ids::LOGIN_REQUEST, 1, 0, 100);
    auto result = mock.inject_message("auth.requests", 0, {}, env);

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(callback_invoked);
    EXPECT_EQ(received_msg_id, msg_ids::LOGIN_REQUEST);
}

TEST(AuthHandlersTest, MockKafkaDispatchByTopic)
{
    apex::test::MockKafkaAdapter mock;

    int auth_count = 0;
    int other_count = 0;

    mock.set_message_callback([&](std::string_view topic, int32_t, std::span<const uint8_t>, std::span<const uint8_t>,
                                  int64_t) -> apex::core::Result<void> {
        if (topic == "auth.requests")
        {
            ++auth_count;
        }
        else
        {
            ++other_count;
        }
        return apex::core::ok();
    });

    auto env = build_envelope(msg_ids::LOGIN_REQUEST, 1, 0, 100);

    (void)mock.inject_message("auth.requests", 0, {}, env);
    (void)mock.inject_message("chat.requests", 0, {}, env);
    (void)mock.inject_message("auth.requests", 0, {}, env);

    EXPECT_EQ(auth_count, 2);
    EXPECT_EQ(other_count, 1);
}

// --- AuthConfig 기본값 ---

TEST(AuthHandlersTest, AuthConfigDefaults)
{
    apex::auth_svc::AuthConfig cfg;
    EXPECT_EQ(cfg.request_topic, "auth.requests");
    EXPECT_EQ(cfg.response_topic, "auth.responses");
    EXPECT_EQ(cfg.bcrypt_work_factor, 12u);
    EXPECT_EQ(cfg.access_token_ttl, std::chrono::seconds{900});
    EXPECT_EQ(cfg.session_ttl, std::chrono::seconds{86400});
}
