// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/error_code.hpp>
#include <apex/shared/adapters/kafka/kafka_config.hpp>
#include <apex/shared/adapters/kafka/kafka_producer.hpp>

#include <gtest/gtest.h>

using namespace apex::shared::adapters::kafka;
using apex::core::ErrorCode;

TEST(KafkaConfig, DefaultValues)
{
    KafkaConfig config;
    EXPECT_EQ(config.brokers, "localhost:9092");
    EXPECT_EQ(config.consumer_group, "apex-group");
    EXPECT_EQ(config.log_topic, "apex-logs");
    EXPECT_EQ(config.compression_type, "lz4");
    EXPECT_EQ(config.producer_batch_ms.count(), 5);
}

TEST(KafkaProducer, ProduceBeforeInitFails)
{
    KafkaConfig config;
    KafkaProducer producer(config);

    EXPECT_FALSE(producer.initialized());
    auto result = producer.produce("test-topic", "key", "value");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::AdapterError);
}

TEST(KafkaProducer, FlushBeforeInitReturnsTrue)
{
    KafkaConfig config;
    KafkaProducer producer(config);

    // flush before init = "nothing to do" -> true
    EXPECT_TRUE(producer.flush(std::chrono::milliseconds{100}));
}

TEST(KafkaProducer, OutqLenBeforeInit)
{
    KafkaConfig config;
    KafkaProducer producer(config);
    EXPECT_EQ(producer.outq_len(), 0);
}

TEST(KafkaProducer, PollBeforeInit)
{
    KafkaConfig config;
    KafkaProducer producer(config);
    EXPECT_EQ(producer.poll(0), 0);
}

TEST(KafkaProducer, InitWithInvalidBroker)
{
    // rd_kafka_new() succeeds even without a real broker (connection is async)
    KafkaConfig config;
    config.brokers = "invalid-broker:99999";
    KafkaProducer producer(config);

    auto result = producer.init();
    // rd_kafka_new() succeeds if config is valid (connection is async)
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(producer.initialized());
}

TEST(KafkaProducer, ProduceAfterInit)
{
    KafkaConfig config;
    config.brokers = "localhost:9092"; // produce enqueue succeeds even without broker
    KafkaProducer producer(config);

    auto init_result = producer.init();
    if (!init_result.has_value())
    {
        GTEST_SKIP() << "librdkafka init failed (expected in some CI environments)";
    }

    // produce() only enqueues internally -- succeeds without broker
    auto before = producer.outq_len() + static_cast<int32_t>(producer.total_produced());
    auto result = producer.produce("test-topic", "key", std::string_view("hello kafka"));
    EXPECT_TRUE(result.has_value());
    producer.poll(100);
    auto after = producer.outq_len() + static_cast<int32_t>(producer.total_produced());
    EXPECT_GT(after, before) << "produce should enqueue at least one message";
}

TEST(KafkaProducer, ProduceWithSpanPayload)
{
    KafkaConfig config;
    KafkaProducer producer(config);
    auto init_result = producer.init();
    if (!init_result.has_value())
    {
        GTEST_SKIP() << "librdkafka init failed";
    }

    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04};
    auto result = producer.produce("test-topic", "key", std::span<const uint8_t>(payload));
    EXPECT_TRUE(result.has_value());
}

TEST(KafkaProducer, StatisticsInitiallyZero)
{
    KafkaConfig config;
    KafkaProducer producer(config);
    EXPECT_EQ(producer.total_produced(), 0u);
    EXPECT_EQ(producer.total_failed(), 0u);
}
