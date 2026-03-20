// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/kafka/kafka_config.hpp>
#include <apex/shared/adapters/kafka/kafka_consumer.hpp>
#include <apex/shared/adapters/kafka/kafka_producer.hpp>

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace apex::shared::adapters::kafka;

class KafkaRoundtripTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        config_.brokers = "localhost:9092";
        config_.consumer_group = "apex-integration-test";
        config_.consume_topics = {"apex-test-roundtrip"};
        config_.consumer_poll_interval = std::chrono::milliseconds{5};
    }

    KafkaConfig config_;
};

TEST_F(KafkaRoundtripTest, ProduceAndConsume)
{
    // --- Producer ---
    KafkaProducer producer(config_);
    auto init_result = producer.init();
    ASSERT_TRUE(init_result.has_value()) << "KafkaProducer init failed - is Kafka running on localhost:9092?";

    const std::string test_key = "integration-key";
    const std::string test_payload = "hello-kafka-roundtrip";

    auto produce_result = producer.produce("apex-test-roundtrip", test_key, test_payload);
    ASSERT_TRUE(produce_result.has_value()) << "produce() failed";

    // Flush to ensure delivery
    ASSERT_TRUE(producer.flush(std::chrono::milliseconds{5000}));
    EXPECT_GE(producer.total_produced(), 1u);

    // --- Consumer ---
    boost::asio::io_context io_ctx;
    KafkaConsumer consumer(config_, 0, io_ctx);

    std::atomic<bool> received{false};
    std::string received_payload;

    consumer.set_message_callback([&](std::string_view /*topic*/, int32_t /*partition*/,
                                      std::span<const uint8_t> /*key*/, std::span<const uint8_t> payload,
                                      int64_t /*offset*/) {
        received_payload = std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
        received.store(true);
    });

    auto consumer_init = consumer.init();
    ASSERT_TRUE(consumer_init.has_value()) << "KafkaConsumer init failed";

    consumer.start_consuming();

    // Run io_context for up to 10 seconds waiting for the message
    io_ctx.run_for(std::chrono::seconds{10});

    consumer.stop_consuming();

    EXPECT_TRUE(received.load()) << "Message not received within 10 seconds";
    if (received.load())
    {
        EXPECT_EQ(received_payload, test_payload);
    }
}
