// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace apex::shared::adapters::kafka
{

/// Kafka adapter configuration. Parsed from TOML [adapters.kafka].
struct KafkaConfig
{
    // Broker connection
    std::string brokers = "localhost:9092";

    // Producer
    std::chrono::milliseconds producer_batch_ms{5}; ///< linger.ms
    uint32_t producer_batch_size = 16384;           ///< batch.size (bytes)
    std::string compression_type = "lz4";           ///< compression.type
    uint32_t message_max_bytes = 1048576;           ///< message.max.bytes (1MB)
    uint32_t queue_buffering_max_messages = 100000; ///< queue.buffering.max.messages
    uint32_t producer_poll_interval_ms = 100;       ///< producer poll timer 주기 (ms)
    uint32_t flush_timeout_ms = 10000;              ///< producer flush 타임아웃 (ms)

    // Consumer
    std::string consumer_group = "apex-group";
    std::vector<std::string> consume_topics;             ///< subscription topic list
    uint32_t consumer_poll_timeout_ms = 0;               ///< rd_kafka_consumer_poll timeout
    std::chrono::milliseconds consumer_poll_interval{5}; ///< Windows poll interval
    int consumer_max_batch = 64;                         ///< poll당 최대 메시지 수

    // Consumer Payload Pool
    size_t payload_pool_initial_count = 64; ///< 사전 할당 버퍼 수
    size_t payload_pool_buffer_size = 4096; ///< 각 버퍼 초기 reserve 크기 (bytes)
    size_t payload_pool_max_count = 4096;   ///< 풀 최대 크기 (0=무제한)

    // KafkaSink
    std::string log_topic = "apex-logs"; ///< log-dedicated topic

    // Common
    std::string client_id = "apex";
};

} // namespace apex::shared::adapters::kafka
