// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/core/scoped_logger.hpp>
#include <apex/shared/adapters/kafka/kafka_config.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#ifndef _WIN32
#include <boost/asio/posix/stream_descriptor.hpp>
#endif

#include <librdkafka/rdkafka.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::shared::adapters::kafka
{

class KafkaProducer; // forward declaration for DLQ injection

/// Received message callback signature.
/// Returns Result<void> — failure triggers DLQ routing if producer is available.
/// @param topic Topic name
/// @param partition Partition number
/// @param key Message key
/// @param payload Message body
/// @param offset Kafka offset
using MessageCallback =
    std::function<apex::core::Result<void>(std::string_view topic, int32_t partition, std::span<const uint8_t> key,
                                           std::span<const uint8_t> payload, int64_t offset)>;

/// Per-core Kafka Consumer instance.
///
/// Partition:core mapping:
/// - Consumer Group distributes partitions
/// - Each core thread's io_context is integrated
///
/// Asio integration:
/// - Linux: rd_kafka_queue_io_event_enable() -> pipe fd -> Asio stream_descriptor
/// - Windows: steady_timer-based periodic polling (5ms default)
class KafkaConsumer : public std::enable_shared_from_this<KafkaConsumer>
{
  public:
    /// @param config Kafka configuration
    /// @param core_id Core ID this Consumer is bound to
    /// @param io_ctx The core's io_context
    /// @param producer DLQ produce target (non-owning, nullptr = no DLQ)
    explicit KafkaConsumer(const KafkaConfig& config, uint32_t core_id, boost::asio::io_context& io_ctx,
                           KafkaProducer* producer = nullptr);
    ~KafkaConsumer();

    KafkaConsumer(const KafkaConsumer&) = delete;
    KafkaConsumer& operator=(const KafkaConsumer&) = delete;

    /// Initialize Consumer + subscribe to topics.
    /// Same consumer group for all cores -- partition auto-distribution.
    [[nodiscard]] apex::core::Result<void> init();

    /// Register message receive callback. Call before init().
    void set_message_callback(MessageCallback cb);

    /// Start consuming by scheduling in Asio event loop.
    /// Linux: pipe fd -> async_wait
    /// Windows: steady_timer polling
    void start_consuming();

    /// Stop consuming (shutdown).
    void stop_consuming();

    /// Whether initialization is complete
    [[nodiscard]] bool initialized() const noexcept
    {
        return rk_ != nullptr;
    }

    /// Whether currently consuming
    [[nodiscard]] bool consuming() const noexcept
    {
        return consuming_;
    }

    /// Statistics
    [[nodiscard]] uint64_t total_consumed() const noexcept
    {
        return metric_consume_total_.load(std::memory_order_relaxed);
    }

    /// Metric atomic access (for MetricsRegistry::counter_from)
    [[nodiscard]] const std::atomic<uint64_t>& metric_consume_total() const noexcept
    {
        return metric_consume_total_;
    }
    [[nodiscard]] const std::atomic<uint64_t>& metric_dlq_total() const noexcept
    {
        return metric_dlq_total_;
    }

  private:
    /// Message polling + callback invocation (non-blocking)
    void poll_messages();

    /// Linux: pipe fd event wait -> message poll loop
    void schedule_async_wait();

    /// Windows: timer-based poll loop
    void schedule_timer_poll();

    apex::core::ScopedLogger logger_{"KafkaConsumer", apex::core::ScopedLogger::NO_CORE, "app"};
    KafkaConfig config_;
    uint32_t core_id_;
    boost::asio::io_context& io_ctx_;

    rd_kafka_t* rk_ = nullptr;
    rd_kafka_queue_t* rkqu_ = nullptr; ///< consumer queue handle

    MessageCallback message_cb_;
    KafkaProducer* producer_ = nullptr; ///< DLQ produce target (non-owning)
    std::atomic<bool> consuming_{false};
    std::atomic<uint64_t> metric_consume_total_{0};
    std::atomic<uint64_t> metric_dlq_total_{0};

#ifndef _WIN32
    // Linux: pipe fd -> Asio stream_descriptor
    int pipe_fds_[2] = {-1, -1};
    std::unique_ptr<boost::asio::posix::stream_descriptor> pipe_desc_;
#endif

    // Windows (or fallback): timer polling
    std::unique_ptr<boost::asio::steady_timer> poll_timer_;
};

} // namespace apex::shared::adapters::kafka
