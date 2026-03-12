#pragma once

#include <apex/shared/adapters/kafka/kafka_config.hpp>
#include <apex/core/result.hpp>

#include <librdkafka/rdkafka.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::shared::adapters::kafka {

/// Global shared Kafka Producer.
///
/// rd_kafka_produce() is thread-safe, so all cores can call directly.
/// No SPSC queue needed -- librdkafka internal queue handles batching.
///
/// Delivery callbacks fire on librdkafka's internal thread,
/// so results are tracked via atomic counters.
///
/// Usage:
///   producer.produce("topic", key, payload);  // fire-and-forget
class KafkaProducer {
public:
    /// @param config Kafka configuration
    explicit KafkaProducer(const KafkaConfig& config);
    ~KafkaProducer();

    KafkaProducer(const KafkaProducer&) = delete;
    KafkaProducer& operator=(const KafkaProducer&) = delete;

    /// Initialize producer. rd_kafka_new() + config application.
    /// @return AdapterError on failure (e.g. invalid config)
    [[nodiscard]] apex::core::Result<void> init();

    /// Fire-and-forget produce. Enqueues to internal queue and returns immediately.
    /// Delivery callback handles error logging.
    /// @param topic Topic name
    /// @param key Message key (partitioning basis, empty allowed)
    /// @param payload Message body
    [[nodiscard]] apex::core::Result<void> produce(
        std::string_view topic,
        std::string_view key,
        std::span<const uint8_t> payload);

    /// Overload: string payload
    [[nodiscard]] apex::core::Result<void> produce(
        std::string_view topic,
        std::string_view key,
        std::string_view payload);

    /// Poll librdkafka internal queue (delivery callback processing).
    /// Must be called periodically for callbacks to fire.
    /// Scheduled by KafkaAdapter::do_init() timer.
    /// @param timeout_ms Poll timeout (0 = non-blocking)
    /// @return Number of events processed
    int poll(int timeout_ms = 0);

    /// Flush all queued messages (graceful shutdown).
    /// @param timeout Maximum wait time
    /// @return Whether flush completed
    [[nodiscard]] bool flush(std::chrono::milliseconds timeout);

    /// Number of messages currently queued
    [[nodiscard]] int32_t outq_len() const noexcept;

    /// Whether initialization is complete
    [[nodiscard]] bool initialized() const noexcept { return rk_ != nullptr; }

    /// Statistics: produced/failed counters
    [[nodiscard]] uint64_t total_produced() const noexcept { return total_produced_; }
    [[nodiscard]] uint64_t total_failed() const noexcept { return total_failed_; }

private:
    /// librdkafka delivery report callback (static, C callback)
    static void delivery_report_cb(rd_kafka_t* rk,
                                   const rd_kafka_message_t* msg,
                                   void* opaque);

    /// rd_kafka_topic_t cache (per topic)
    rd_kafka_topic_t* get_or_create_topic(std::string_view topic);

    KafkaConfig config_;
    rd_kafka_t* rk_ = nullptr;                          ///< librdkafka handle

    /// Topic handle cache (rd_kafka_topic_new is relatively heavy)
    struct TopicEntry {
        std::string name;
        rd_kafka_topic_t* rkt = nullptr;
    };
    std::vector<TopicEntry> topic_cache_;
    mutable std::mutex topic_mutex_;                    ///< Protects topic_cache_

    // Statistics (atomic -- concurrent produce from multiple cores)
    std::atomic<uint64_t> total_produced_{0};
    std::atomic<uint64_t> total_failed_{0};
};

} // namespace apex::shared::adapters::kafka
