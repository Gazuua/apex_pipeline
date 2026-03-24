// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/scoped_logger.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/kafka/consumer_payload_pool.hpp>
#include <apex/shared/adapters/kafka/kafka_config.hpp>
#include <apex/shared/adapters/kafka/kafka_consumer.hpp>
#include <apex/shared/adapters/kafka/kafka_producer.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace apex::shared::adapters::kafka
{

/// Kafka adapter. Inherits AdapterBase CRTP.
///
/// Registered as a single global instance to Server, internally managing:
/// - KafkaProducer x1 (global shared, thread-safe)
/// - KafkaConsumer xN (one per core, partition:core mapping)
///
/// Usage:
///   Server({.port = 9000, .num_cores = 4})
///       .add_adapter<KafkaAdapter>(kafka_config)
///       .add_service<MyService>()
///       .run();
///
///   // From service:
///   auto& kafka = server.adapter<KafkaAdapter>();
///   kafka.produce("topic", key, payload);
class KafkaAdapter : public apex::shared::adapters::AdapterBase<KafkaAdapter>
{
  public:
    explicit KafkaAdapter(KafkaConfig config);
    ~KafkaAdapter();

    // --- AdapterBase CRTP interface ---

    /// Per-core resource initialization.
    /// Producer init + per-core Consumer create/init + poll timer start.
    void do_init(apex::core::CoreEngine& engine);

    /// Reject new produce requests, stop Consumer consumption.
    void do_drain();

    /// Producer flush + Consumer close + resource cleanup.
    void do_close();

    /// Adapter name
    [[nodiscard]] std::string_view do_name() const noexcept
    {
        return "kafka";
    }

    // --- Kafka API (used by services) ---

    /// Fire-and-forget produce.
    [[nodiscard]] apex::core::Result<void> produce(std::string_view topic, std::string_view key,
                                                   std::span<const uint8_t> payload);

    /// Overload: string payload
    [[nodiscard]] apex::core::Result<void> produce(std::string_view topic, std::string_view key,
                                                   std::string_view payload);

    /// [D2] Adapter-service 자동 배선. has_kafka_handlers() 서비스를 감지하여
    /// KafkaDispatchBridge를 자동 생성하고 consumer 콜백에 연결.
    void wire_services(std::vector<std::unique_ptr<apex::core::ServiceBaseInterface>>& services,
                       apex::core::CoreEngine& engine);

    /// Register Consumer message callback.
    /// Sets the same callback on each core's Consumer.
    /// Must be set before do_init() is called.
    void set_message_callback(MessageCallback cb);

    /// Producer access (for direct control)
    [[nodiscard]] KafkaProducer& producer() noexcept
    {
        return *producer_;
    }
    [[nodiscard]] const KafkaProducer& producer() const noexcept
    {
        return *producer_;
    }

    /// Per-core Consumer access
    [[nodiscard]] KafkaConsumer& consumer(uint32_t core_id);

    /// Config access
    [[nodiscard]] const KafkaConfig& config() const noexcept
    {
        return config_;
    }

    /// Consumer payload pool access (for external Kafka callback users)
    [[nodiscard]] ConsumerPayloadPool& payload_pool() noexcept
    {
        return payload_pool_;
    }
    [[nodiscard]] const ConsumerPayloadPool& payload_pool() const noexcept
    {
        return payload_pool_;
    }

  private:
    /// Producer poll timer (delivery callback processing).
    /// Runs on core 0's io_context periodically.
    void start_producer_poll_timer(boost::asio::io_context& io_ctx);
    void on_producer_poll_tick();

    apex::core::ScopedLogger logger_{"KafkaAdapter", apex::core::ScopedLogger::NO_CORE, "app"};
    KafkaConfig config_;
    MessageCallback message_cb_;
    ConsumerPayloadPool payload_pool_;

    std::unique_ptr<KafkaProducer> producer_;
    std::vector<std::shared_ptr<KafkaConsumer>> consumers_;

    // Producer poll timer
    std::unique_ptr<boost::asio::steady_timer> producer_poll_timer_;
};

} // namespace apex::shared::adapters::kafka
