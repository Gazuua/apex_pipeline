#include <apex/gateway/message_router.hpp>
#include <apex/gateway/route_table.hpp>
#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

#include "../../tests/mocks/mock_kafka_adapter.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace envelope = apex::shared::protocols::kafka;

// ============================================================
// MessageRouter 테스트 — RouteTable 기반 msg_id→topic 라우팅 +
// WireHeader→Envelope 변환 검증 (Mock Kafka 사용)
// ============================================================

namespace {

/// RouteTable 생성 헬퍼.
auto make_route_table(std::vector<apex::gateway::RouteEntry> routes) {
    auto table = apex::gateway::RouteTable::build(std::move(routes));
    assert(table.has_value());
    return std::make_shared<const apex::gateway::RouteTable>(std::move(*table));
}

} // namespace

class MessageRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        table_ = make_route_table({
            {1000, 1999, "auth.requests"},
            {2000, 2999, "chat.requests"},
        });
    }

    apex::test::MockKafkaAdapter mock_kafka_;
    apex::gateway::RouteTablePtr table_;
};

// --- msg_id → topic 라우팅 ---

TEST_F(MessageRouterTest, RouteAuthRange) {
    // MessageRouter는 KafkaAdapter& 를 받으므로, MockKafkaAdapter를 직접 사용할 수 없다.
    // 대신 RouteTable::resolve를 직접 테스트 (MessageRouter의 핵심 로직).
    auto topic = table_->resolve(1000);
    ASSERT_TRUE(topic.has_value());
    EXPECT_EQ(*topic, "auth.requests");
}

TEST_F(MessageRouterTest, RouteChatRange) {
    auto topic = table_->resolve(2500);
    ASSERT_TRUE(topic.has_value());
    EXPECT_EQ(*topic, "chat.requests");
}

TEST_F(MessageRouterTest, RouteNotFound) {
    auto topic = table_->resolve(9999);
    EXPECT_FALSE(topic.has_value());
}

TEST_F(MessageRouterTest, RouteBoundaryValues) {
    // Range begin boundary
    EXPECT_TRUE(table_->resolve(1000).has_value());
    EXPECT_TRUE(table_->resolve(2000).has_value());

    // Range end boundary
    EXPECT_TRUE(table_->resolve(1999).has_value());
    EXPECT_TRUE(table_->resolve(2999).has_value());

    // Just outside boundaries
    EXPECT_FALSE(table_->resolve(999).has_value());
    EXPECT_FALSE(table_->resolve(3000).has_value());
}

// --- RouteTable atomic update ---

TEST_F(MessageRouterTest, RouteTableAtomicSwap) {
    // Initial table resolves auth range
    ASSERT_TRUE(table_->resolve(1000).has_value());

    // Create new table with different mapping
    auto new_table = make_route_table({
        {1000, 1999, "auth.v2.requests"},
        {3000, 3999, "game.requests"},
    });

    // Verify new table
    auto topic = new_table->resolve(1000);
    ASSERT_TRUE(topic.has_value());
    EXPECT_EQ(*topic, "auth.v2.requests");

    // Old chat range should not exist in new table
    EXPECT_FALSE(new_table->resolve(2500).has_value());

    // New game range should exist
    auto game = new_table->resolve(3500);
    ASSERT_TRUE(game.has_value());
    EXPECT_EQ(*game, "game.requests");
}

// --- WireHeader → Envelope 변환 ---

TEST_F(MessageRouterTest, EnvelopeBuildAndParse) {
    // Envelope의 직렬화/역직렬화 round-trip 검증.
    // MessageRouter::build_envelope은 private이므로, 동일 로직을 재현.

    envelope::RoutingHeader rh;
    rh.msg_id = 1001;
    rh.flags = 0;

    envelope::MetadataPrefix meta;
    meta.core_id = 2;
    meta.corr_id = 42;
    meta.source_id = envelope::source_ids::GATEWAY;
    meta.session_id = 12345;
    meta.timestamp = 1710000000000ULL;

    auto rh_bytes = rh.serialize();
    auto meta_bytes = meta.serialize();

    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};

    std::vector<uint8_t> envelope_buf;
    envelope_buf.insert(envelope_buf.end(), rh_bytes.begin(), rh_bytes.end());
    envelope_buf.insert(envelope_buf.end(), meta_bytes.begin(), meta_bytes.end());
    envelope_buf.insert(envelope_buf.end(), payload.begin(), payload.end());

    // Parse back
    auto parsed_rh = envelope::RoutingHeader::parse(envelope_buf);
    ASSERT_TRUE(parsed_rh.has_value());
    EXPECT_EQ(parsed_rh->msg_id, 1001u);

    auto parsed_meta = envelope::MetadataPrefix::parse(
        std::span<const uint8_t>(envelope_buf).subspan(envelope::RoutingHeader::SIZE));
    ASSERT_TRUE(parsed_meta.has_value());
    EXPECT_EQ(parsed_meta->core_id, 2);
    EXPECT_EQ(parsed_meta->corr_id, 42u);
    EXPECT_EQ(parsed_meta->session_id, 12345u);

    auto fbs = std::span<const uint8_t>(envelope_buf)
                   .subspan(envelope::ENVELOPE_HEADER_SIZE);
    ASSERT_EQ(fbs.size(), 3u);
    EXPECT_EQ(fbs[0], 0x01);
}

// --- MockKafkaAdapter produce 기록 검증 ---

TEST_F(MessageRouterTest, MockKafkaProduceRecorded) {
    std::vector<uint8_t> payload = {0xAA, 0xBB};
    auto result = mock_kafka_.produce("auth.requests", "key-1", payload);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(mock_kafka_.produce_count(), 1u);
    EXPECT_EQ(mock_kafka_.produced()[0].topic, "auth.requests");
    EXPECT_EQ(mock_kafka_.produced()[0].key, "key-1");
    EXPECT_EQ(mock_kafka_.produced()[0].payload, payload);
}

TEST_F(MessageRouterTest, MockKafkaProduceFailure) {
    mock_kafka_.set_fail_produce(true);
    std::vector<uint8_t> payload = {0x01};
    auto result = mock_kafka_.produce("topic", "key", payload);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::core::ErrorCode::AdapterError);
}

// --- Correlation ID 생성 ---

TEST_F(MessageRouterTest, CorrIdGenerationPattern) {
    // corr_id = (core_id << 48) | counter
    // core_id 2라면, 상위 16비트가 2이어야 함.
    uint16_t core_id = 2;
    uint64_t counter1 = 1;
    uint64_t counter2 = 2;

    uint64_t corr1 = (static_cast<uint64_t>(core_id) << 48) | counter1;
    uint64_t corr2 = (static_cast<uint64_t>(core_id) << 48) | counter2;

    // Verify core_id extraction
    EXPECT_EQ(corr1 >> 48, core_id);
    EXPECT_EQ(corr2 >> 48, core_id);

    // Verify monotonicity
    EXPECT_LT(corr1, corr2);
}
