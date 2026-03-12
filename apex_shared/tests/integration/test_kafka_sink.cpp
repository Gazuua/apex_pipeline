#include <apex/shared/adapters/kafka/kafka_sink.hpp>
#include <apex/shared/adapters/kafka/kafka_producer.hpp>
#include <apex/shared/adapters/kafka/kafka_consumer.hpp>
#include <apex/shared/adapters/kafka/kafka_config.hpp>

#include <gtest/gtest.h>

#include <spdlog/spdlog.h>
#include <spdlog/logger.h>

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>

using namespace apex::shared::adapters::kafka;

class KafkaSinkIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.brokers = "localhost:9092";
        config_.consumer_group = "apex-sink-integration-test";
        config_.consume_topics = {"apex-sink-test"};
        config_.consumer_poll_interval = std::chrono::milliseconds{5};
    }

    KafkaConfig config_;
};

TEST_F(KafkaSinkIntegrationTest, SpdlogToKafkaTopic) {
    // --- Producer + KafkaSink ---
    KafkaProducer producer(config_);
    auto init_result = producer.init();
    ASSERT_TRUE(init_result.has_value())
        << "KafkaProducer init failed - is Kafka running on localhost:9092?";

    auto kafka_sink = std::make_shared<KafkaSink>(producer, "apex-sink-test");
    auto logger = std::make_shared<spdlog::logger>("apex-sink-test", kafka_sink);
    logger->set_level(spdlog::level::info);

    // Log a message through spdlog -> KafkaSink -> Kafka topic
    logger->info("integration-test-message");
    logger->flush();

    // Poll delivery callbacks
    producer.poll(100);
    ASSERT_TRUE(producer.flush(std::chrono::milliseconds{5000}));
    EXPECT_GE(producer.total_produced(), 1u);

    // --- Consumer: verify the message arrives on the topic ---
    boost::asio::io_context io_ctx;
    KafkaConsumer consumer(config_, 0, io_ctx);

    std::atomic<bool> received{false};
    std::string received_payload;

    consumer.set_message_callback(
        [&](std::string_view /*topic*/, int32_t /*partition*/,
            std::span<const uint8_t> /*key*/,
            std::span<const uint8_t> payload,
            int64_t /*offset*/) {
            std::string msg(
                reinterpret_cast<const char*>(payload.data()), payload.size());
            // KafkaSink formats as JSON with "msg" field
            if (msg.find("integration-test-message") != std::string::npos) {
                received_payload = std::move(msg);
                received.store(true);
            }
        });

    auto consumer_init = consumer.init();
    ASSERT_TRUE(consumer_init.has_value())
        << "KafkaConsumer init failed";

    consumer.start_consuming();

    // Run io_context for up to 10 seconds waiting for the message
    io_ctx.run_for(std::chrono::seconds{10});

    consumer.stop_consuming();

    EXPECT_TRUE(received.load())
        << "KafkaSink message not received within 10 seconds";
    if (received.load()) {
        // Verify JSON contains the log message
        EXPECT_NE(received_payload.find("integration-test-message"), std::string::npos);
        // Verify JSON structure (should contain "level" and "msg" fields)
        EXPECT_NE(received_payload.find("\"level\""), std::string::npos);
        EXPECT_NE(received_payload.find("\"msg\""), std::string::npos);
    }
}
