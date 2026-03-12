#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace apex::shared::adapters::kafka {

/// Kafka adapter configuration. Parsed from TOML [adapters.kafka].
struct KafkaConfig {
    // Broker connection
    std::string brokers = "localhost:9092";

    // Producer
    std::chrono::milliseconds producer_batch_ms{5};     ///< linger.ms
    uint32_t producer_batch_size = 16384;               ///< batch.size (bytes)
    std::string compression_type = "lz4";               ///< compression.type
    uint32_t message_max_bytes = 1048576;               ///< message.max.bytes (1MB)
    uint32_t queue_buffering_max_messages = 100000;     ///< queue.buffering.max.messages

    // Consumer
    std::string consumer_group = "apex-group";
    std::vector<std::string> consume_topics;            ///< subscription topic list
    uint32_t consumer_poll_timeout_ms = 0;              ///< rd_kafka_consumer_poll timeout
    std::chrono::milliseconds consumer_poll_interval{5};///< Windows poll interval

    // KafkaSink
    std::string log_topic = "apex-logs";                ///< log-dedicated topic

    // Common
    std::string client_id = "apex";
};

} // namespace apex::shared::adapters::kafka
