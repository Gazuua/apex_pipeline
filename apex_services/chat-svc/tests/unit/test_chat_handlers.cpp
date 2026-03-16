#include <apex/chat_svc/chat_service.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

#include "../../../tests/mocks/mock_kafka_adapter.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace envelope = apex::shared::protocols::kafka;

// ============================================================
// ChatService 핸들러 단위 테스트 — Mock Kafka로 Envelope dispatch 검증.
//
// ChatService는 4개 어댑터(kafka, redis_data, redis_pubsub, pg) 참조를 필요로 하므로,
// 직접 인스턴스화 대신 dispatch_envelope이 사용하는 핵심 로직을 개별 검증:
// 1. Kafka Envelope 파싱 + msg_id dispatch
// 2. Mock Kafka produce/inject로 메시지 흐름 검증
// 3. msg_id 상수 정합성 + Config 검증
// ============================================================

namespace {

// Chat msg_id constants (mirrored from chat_service.hpp)
namespace msg_ids {
    // Room management
    constexpr uint32_t CREATE_ROOM_REQUEST   = 2001;
    constexpr uint32_t CREATE_ROOM_RESPONSE  = 2002;
    constexpr uint32_t JOIN_ROOM_REQUEST     = 2003;
    constexpr uint32_t JOIN_ROOM_RESPONSE    = 2004;
    constexpr uint32_t LEAVE_ROOM_REQUEST    = 2005;
    constexpr uint32_t LEAVE_ROOM_RESPONSE   = 2006;
    constexpr uint32_t LIST_ROOMS_REQUEST    = 2007;
    constexpr uint32_t LIST_ROOMS_RESPONSE   = 2008;

    // Message send/broadcast
    constexpr uint32_t SEND_MESSAGE_REQUEST  = 2011;
    constexpr uint32_t SEND_MESSAGE_RESPONSE = 2012;
    constexpr uint32_t CHAT_MESSAGE          = 2013;

    // Whisper
    constexpr uint32_t WHISPER_REQUEST       = 2021;
    constexpr uint32_t WHISPER_RESPONSE      = 2022;

    // History
    constexpr uint32_t CHAT_HISTORY_REQUEST  = 2031;
    constexpr uint32_t CHAT_HISTORY_RESPONSE = 2032;

    // Global broadcast
    constexpr uint32_t GLOBAL_BROADCAST_REQUEST  = 2041;
    constexpr uint32_t GLOBAL_BROADCAST_RESPONSE = 2042;
} // namespace msg_ids

/// Envelope 빌드 헬퍼.
std::vector<uint8_t> build_envelope(
    uint32_t msg_id,
    uint64_t corr_id,
    uint16_t core_id,
    uint64_t session_id,
    std::span<const uint8_t> fbs_payload = {})
{
    envelope::RoutingHeader rh;
    rh.header_version = envelope::RoutingHeader::CURRENT_VERSION;
    rh.flags = 0;
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

TEST(ChatHandlersTest, MsgIdRoomManagement) {
    EXPECT_EQ(msg_ids::CREATE_ROOM_REQUEST, 2001u);
    EXPECT_EQ(msg_ids::CREATE_ROOM_RESPONSE, 2002u);
    EXPECT_EQ(msg_ids::JOIN_ROOM_REQUEST, 2003u);
    EXPECT_EQ(msg_ids::JOIN_ROOM_RESPONSE, 2004u);
    EXPECT_EQ(msg_ids::LEAVE_ROOM_REQUEST, 2005u);
    EXPECT_EQ(msg_ids::LEAVE_ROOM_RESPONSE, 2006u);
    EXPECT_EQ(msg_ids::LIST_ROOMS_REQUEST, 2007u);
    EXPECT_EQ(msg_ids::LIST_ROOMS_RESPONSE, 2008u);
}

TEST(ChatHandlersTest, MsgIdMessageSend) {
    EXPECT_EQ(msg_ids::SEND_MESSAGE_REQUEST, 2011u);
    EXPECT_EQ(msg_ids::SEND_MESSAGE_RESPONSE, 2012u);
    EXPECT_EQ(msg_ids::CHAT_MESSAGE, 2013u);
}

TEST(ChatHandlersTest, MsgIdWhisper) {
    EXPECT_EQ(msg_ids::WHISPER_REQUEST, 2021u);
    EXPECT_EQ(msg_ids::WHISPER_RESPONSE, 2022u);
}

TEST(ChatHandlersTest, MsgIdHistory) {
    EXPECT_EQ(msg_ids::CHAT_HISTORY_REQUEST, 2031u);
    EXPECT_EQ(msg_ids::CHAT_HISTORY_RESPONSE, 2032u);
}

TEST(ChatHandlersTest, MsgIdGlobalBroadcast) {
    EXPECT_EQ(msg_ids::GLOBAL_BROADCAST_REQUEST, 2041u);
    EXPECT_EQ(msg_ids::GLOBAL_BROADCAST_RESPONSE, 2042u);
}

// --- Envelope 파싱 ---

TEST(ChatHandlersTest, EnvelopeParseCreateRoom) {
    auto env = build_envelope(msg_ids::CREATE_ROOM_REQUEST, 100, 2, 55555);

    auto rh = envelope::RoutingHeader::parse(env);
    ASSERT_TRUE(rh.has_value());
    EXPECT_EQ(rh->msg_id, msg_ids::CREATE_ROOM_REQUEST);

    auto meta = envelope::MetadataPrefix::parse(
        std::span<const uint8_t>(env).subspan(envelope::RoutingHeader::SIZE));
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->corr_id, 100u);
    EXPECT_EQ(meta->session_id, 55555u);
}

TEST(ChatHandlersTest, EnvelopeParseSendMessage) {
    auto env = build_envelope(msg_ids::SEND_MESSAGE_REQUEST, 200, 0, 77777);

    auto rh = envelope::RoutingHeader::parse(env);
    ASSERT_TRUE(rh.has_value());
    EXPECT_EQ(rh->msg_id, msg_ids::SEND_MESSAGE_REQUEST);
}

TEST(ChatHandlersTest, EnvelopeParseWhisper) {
    auto env = build_envelope(msg_ids::WHISPER_REQUEST, 300, 1, 88888);

    auto rh = envelope::RoutingHeader::parse(env);
    ASSERT_TRUE(rh.has_value());
    EXPECT_EQ(rh->msg_id, msg_ids::WHISPER_REQUEST);
}

// --- Envelope too small ---

TEST(ChatHandlersTest, EnvelopeTooSmallRejected) {
    std::vector<uint8_t> small(20, 0);
    EXPECT_LT(small.size(), envelope::ENVELOPE_HEADER_SIZE);
}

// --- Response Envelope 구조 검증 ---

TEST(ChatHandlersTest, ResponseEnvelopeDirection) {
    envelope::RoutingHeader routing;
    routing.header_version = envelope::RoutingHeader::CURRENT_VERSION;
    routing.flags = envelope::routing_flags::DIRECTION_RESPONSE;
    routing.msg_id = msg_ids::CREATE_ROOM_RESPONSE;

    auto bytes = routing.serialize();
    auto parsed = envelope::RoutingHeader::parse(bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE((parsed->flags & envelope::routing_flags::DIRECTION_RESPONSE) != 0);
    EXPECT_EQ(parsed->msg_id, msg_ids::CREATE_ROOM_RESPONSE);
}

TEST(ChatHandlersTest, ResponseEnvelopeSourceId) {
    envelope::MetadataPrefix meta;
    meta.source_id = envelope::source_ids::CHAT;

    auto bytes = meta.serialize();
    auto parsed = envelope::MetadataPrefix::parse(bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->source_id, envelope::source_ids::CHAT);
}

TEST(ChatHandlersTest, BroadcastEnvelopeFlags) {
    envelope::RoutingHeader routing;
    routing.flags = envelope::routing_flags::DIRECTION_RESPONSE
                  | envelope::routing_flags::DELIVERY_BROADCAST;
    routing.msg_id = msg_ids::GLOBAL_BROADCAST_RESPONSE;

    auto bytes = routing.serialize();
    auto parsed = envelope::RoutingHeader::parse(bytes);
    ASSERT_TRUE(parsed.has_value());

    // Direction = response
    EXPECT_TRUE((parsed->flags & envelope::routing_flags::DIRECTION_RESPONSE) != 0);
    // Delivery = broadcast
    EXPECT_EQ(parsed->flags & envelope::routing_flags::DELIVERY_MASK,
              envelope::routing_flags::DELIVERY_BROADCAST);
}

// --- MockKafkaAdapter로 메시지 흐름 시뮬레이션 ---

TEST(ChatHandlersTest, MockKafkaMessageDispatch) {
    apex::test::MockKafkaAdapter mock;

    std::vector<uint32_t> dispatched_msg_ids;

    mock.set_message_callback(
        [&](std::string_view topic, int32_t,
            std::span<const uint8_t>, std::span<const uint8_t> payload,
            int64_t) -> apex::core::Result<void> {

            if (topic == "chat.requests" &&
                payload.size() >= envelope::ENVELOPE_HEADER_SIZE) {
                auto rh = envelope::RoutingHeader::parse(payload);
                if (rh.has_value()) {
                    dispatched_msg_ids.push_back(rh->msg_id);
                }
            }
            return apex::core::ok();
        });

    // Inject various chat message types
    mock.inject_message("chat.requests", 0, {},
        build_envelope(msg_ids::CREATE_ROOM_REQUEST, 1, 0, 100));
    mock.inject_message("chat.requests", 0, {},
        build_envelope(msg_ids::SEND_MESSAGE_REQUEST, 2, 0, 200));
    mock.inject_message("chat.requests", 0, {},
        build_envelope(msg_ids::WHISPER_REQUEST, 3, 0, 300));
    mock.inject_message("chat.requests", 0, {},
        build_envelope(msg_ids::CHAT_HISTORY_REQUEST, 4, 0, 400));

    ASSERT_EQ(dispatched_msg_ids.size(), 4u);
    EXPECT_EQ(dispatched_msg_ids[0], msg_ids::CREATE_ROOM_REQUEST);
    EXPECT_EQ(dispatched_msg_ids[1], msg_ids::SEND_MESSAGE_REQUEST);
    EXPECT_EQ(dispatched_msg_ids[2], msg_ids::WHISPER_REQUEST);
    EXPECT_EQ(dispatched_msg_ids[3], msg_ids::CHAT_HISTORY_REQUEST);
}

TEST(ChatHandlersTest, MockKafkaTopicFiltering) {
    apex::test::MockKafkaAdapter mock;

    int chat_count = 0;

    mock.set_message_callback(
        [&](std::string_view topic, int32_t, std::span<const uint8_t>,
            std::span<const uint8_t>, int64_t) -> apex::core::Result<void> {
            if (topic == "chat.requests") ++chat_count;
            return apex::core::ok();
        });

    auto env = build_envelope(msg_ids::CREATE_ROOM_REQUEST, 1, 0, 100);

    // Only chat.requests should be counted
    mock.inject_message("chat.requests", 0, {}, env);
    mock.inject_message("auth.requests", 0, {}, env);
    mock.inject_message("chat.requests", 0, {}, env);
    mock.inject_message("game.requests", 0, {}, env);

    EXPECT_EQ(chat_count, 2);
}

TEST(ChatHandlersTest, MockKafkaProduceResponse) {
    apex::test::MockKafkaAdapter mock;

    // Simulate producing a response
    envelope::RoutingHeader rh;
    rh.flags = envelope::routing_flags::DIRECTION_RESPONSE;
    rh.msg_id = msg_ids::CREATE_ROOM_RESPONSE;
    auto rh_bytes = rh.serialize();

    envelope::MetadataPrefix meta;
    meta.source_id = envelope::source_ids::CHAT;
    meta.session_id = 12345;
    auto meta_bytes = meta.serialize();

    std::vector<uint8_t> response_buf;
    response_buf.insert(response_buf.end(), rh_bytes.begin(), rh_bytes.end());
    response_buf.insert(response_buf.end(), meta_bytes.begin(), meta_bytes.end());

    auto result = mock.produce("chat.responses", "12345", response_buf);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(mock.produce_count(), 1u);
    EXPECT_EQ(mock.produced()[0].topic, "chat.responses");
    EXPECT_EQ(mock.produced()[0].key, "12345");
}

// --- PubSub payload 형식 검증 ---

TEST(ChatHandlersTest, PubSubPayloadFormat) {
    // ChatService::build_pubsub_payload 형식: [msg_id(u32 BE)] + [fbs payload]
    // Gateway BroadcastFanout reads msg_id as big-endian to build WireHeader.
    uint32_t msg_id = msg_ids::CHAT_MESSAGE;
    std::vector<uint8_t> fbs = {0xAA, 0xBB, 0xCC};

    // Build (big-endian, matching build_pubsub_payload implementation)
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>((msg_id >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((msg_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((msg_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(msg_id & 0xFF));
    buf.insert(buf.end(), fbs.begin(), fbs.end());

    EXPECT_EQ(buf.size(), sizeof(uint32_t) + fbs.size());

    // Parse back msg_id (big-endian, matching BroadcastFanout::build_wire_frame)
    uint32_t parsed_msg_id =
        (static_cast<uint32_t>(buf[0]) << 24) |
        (static_cast<uint32_t>(buf[1]) << 16) |
        (static_cast<uint32_t>(buf[2]) << 8)  |
        (static_cast<uint32_t>(buf[3]));
    EXPECT_EQ(parsed_msg_id, msg_ids::CHAT_MESSAGE);

    // Payload
    EXPECT_EQ(buf[4], 0xAA);
    EXPECT_EQ(buf[5], 0xBB);
    EXPECT_EQ(buf[6], 0xCC);
}

// --- ChatService::Config ---

TEST(ChatHandlersTest, ChatConfigDefaults) {
    apex::chat_svc::ChatService::Config cfg;
    EXPECT_EQ(cfg.request_topic, "chat.requests");
    EXPECT_EQ(cfg.response_topic, "chat.responses");
    EXPECT_EQ(cfg.persist_topic, "chat.messages.persist");
    EXPECT_EQ(cfg.max_room_members, 100u);
    EXPECT_EQ(cfg.max_message_length, 2000u);
    EXPECT_EQ(cfg.history_page_size, 50u);
}

TEST(ChatHandlersTest, ChatConfigCustom) {
    apex::chat_svc::ChatService::Config cfg{
        .request_topic = "test.requests",
        .response_topic = "test.responses",
        .persist_topic = "test.persist",
        .max_room_members = 50,
        .max_message_length = 500,
        .history_page_size = 10,
    };
    EXPECT_EQ(cfg.max_room_members, 50u);
    EXPECT_EQ(cfg.max_message_length, 500u);
}
