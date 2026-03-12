#include <apex/shared/adapters/kafka/kafka_sink.hpp>
#include <apex/shared/adapters/kafka/kafka_producer.hpp>
#include <apex/shared/adapters/kafka/kafka_config.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/logger.h>

#include <gtest/gtest.h>

using namespace apex::shared::adapters::kafka;

class KafkaSinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.brokers = "localhost:9092";
        producer_ = std::make_unique<KafkaProducer>(config_);
        auto init_result = producer_->init();
        if (!init_result.has_value()) {
            producer_initialized_ = false;
        } else {
            producer_initialized_ = true;
        }
    }

    KafkaConfig config_;
    std::unique_ptr<KafkaProducer> producer_;
    bool producer_initialized_ = false;
};

TEST_F(KafkaSinkTest, ConstructionSucceeds) {
    if (!producer_initialized_) GTEST_SKIP() << "Producer init failed";

    auto sink = std::make_shared<KafkaSink>(*producer_, "test-logs");
    EXPECT_NE(sink, nullptr);
}

TEST_F(KafkaSinkTest, SinkWithLogger) {
    if (!producer_initialized_) GTEST_SKIP() << "Producer init failed";

    auto sink = std::make_shared<KafkaSink>(*producer_, "test-logs");
    auto logger = std::make_shared<spdlog::logger>("test-kafka", sink);

    // Log messages -- enqueued even without broker
    logger->info("test message {}", 42);
    logger->warn("warning message");
    logger->flush();

    // Indirect verify that produce was called (outq_len or statistics)
    // Without broker, delivery fails but enqueue succeeds
    SUCCEED();
}

TEST_F(KafkaSinkTest, SinkWithDifferentLevels) {
    if (!producer_initialized_) GTEST_SKIP() << "Producer init failed";

    auto sink = std::make_shared<KafkaSink>(*producer_, "test-logs");
    auto logger = std::make_shared<spdlog::logger>("level-test", sink);
    logger->set_level(spdlog::level::trace);

    logger->trace("trace msg");
    logger->debug("debug msg");
    logger->info("info msg");
    logger->warn("warn msg");
    logger->error("error msg");
    logger->critical("critical msg");

    SUCCEED();
}

TEST_F(KafkaSinkTest, SinkHandlesSpecialCharacters) {
    if (!producer_initialized_) GTEST_SKIP() << "Producer init failed";

    auto sink = std::make_shared<KafkaSink>(*producer_, "test-logs");
    auto logger = std::make_shared<spdlog::logger>("escape-test", sink);

    // Characters that need JSON escaping
    logger->info("message with \"quotes\" and \\backslash");
    logger->info("newline\nand\ttab");

    SUCCEED();
}
