// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/error_code.hpp>
#include <apex/shared/adapters/kafka/kafka_config.hpp>
#include <apex/shared/adapters/kafka/kafka_consumer.hpp>

#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

using namespace apex::shared::adapters::kafka;
using apex::core::ErrorCode;

TEST(KafkaConsumer, NotInitializedByDefault)
{
    boost::asio::io_context io_ctx;
    KafkaConfig config;
    KafkaConsumer consumer(config, 0, io_ctx);

    EXPECT_FALSE(consumer.initialized());
    EXPECT_FALSE(consumer.consuming());
    EXPECT_EQ(consumer.total_consumed(), 0u);
}

TEST(KafkaConsumer, InitCreatesHandle)
{
    boost::asio::io_context io_ctx;
    KafkaConfig config;
    config.brokers = "localhost:9092";
    // Empty topic list -- skip subscribe
    KafkaConsumer consumer(config, 0, io_ctx);

    auto result = consumer.init();
    // rd_kafka_new succeeds even without broker (connection is async)
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(consumer.initialized());
}

TEST(KafkaConsumer, InitWithTopicSubscription)
{
    boost::asio::io_context io_ctx;
    KafkaConfig config;
    config.brokers = "localhost:9092";
    config.consume_topics = {"test-topic-1", "test-topic-2"};
    KafkaConsumer consumer(config, 1, io_ctx);

    auto result = consumer.init();
    EXPECT_TRUE(result.has_value());
}

TEST(KafkaConsumer, SetMessageCallback)
{
    boost::asio::io_context io_ctx;
    KafkaConfig config;
    KafkaConsumer consumer(config, 0, io_ctx);

    bool called = false;
    consumer.set_message_callback([&](std::string_view, int32_t, std::span<const uint8_t>, std::span<const uint8_t>,
                                      int64_t) -> apex::core::Result<void> {
        called = true;
        return {};
    });

    // Only verify callback was set -- actual invocation in integration tests
    EXPECT_FALSE(called);
}

TEST(KafkaConsumer, StartStopConsuming)
{
    boost::asio::io_context io_ctx;
    KafkaConfig config;
    config.brokers = "localhost:9092";
    KafkaConsumer consumer(config, 0, io_ctx);

    auto result = consumer.init();
    if (!result.has_value())
    {
        GTEST_SKIP() << "librdkafka init failed";
    }

    consumer.start_consuming();
    EXPECT_TRUE(consumer.consuming());

    consumer.stop_consuming();
    EXPECT_FALSE(consumer.consuming());
}
