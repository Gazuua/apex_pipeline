#pragma once

#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/kafka/kafka_config.hpp>
#include <apex/shared/adapters/kafka/kafka_producer.hpp>
#include <apex/shared/adapters/kafka/kafka_consumer.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace apex::shared::adapters::kafka {

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
class KafkaAdapter : public apex::shared::adapters::AdapterBase<KafkaAdapter> {
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
    [[nodiscard]] std::string_view do_name() const noexcept { return "kafka"; }

    // --- Kafka API (used by services) ---

    /// Fire-and-forget produce.
    [[nodiscard]] apex::core::Result<void> produce(
        std::string_view topic,
        std::string_view key,
        std::span<const uint8_t> payload);

    /// Overload: string payload
    [[nodiscard]] apex::core::Result<void> produce(
        std::string_view topic,
        std::string_view key,
        std::string_view payload);

    /// Register Consumer message callback.
    /// Sets the same callback on each core's Consumer.
    /// Must be set before do_init() is called.
    void set_message_callback(MessageCallback cb);

    /// Producer access (for direct control)
    [[nodiscard]] KafkaProducer& producer() noexcept { return *producer_; }
    [[nodiscard]] const KafkaProducer& producer() const noexcept { return *producer_; }

    /// Per-core Consumer access
    [[nodiscard]] KafkaConsumer& consumer(uint32_t core_id);

    /// Config access
    [[nodiscard]] const KafkaConfig& config() const noexcept { return config_; }

private:
    /// Producer poll timer (delivery callback processing).
    /// Runs on core 0's io_context periodically.
    void start_producer_poll_timer(boost::asio::io_context& io_ctx);
    void on_producer_poll_tick();

    KafkaConfig config_;
    MessageCallback message_cb_;

    std::unique_ptr<KafkaProducer> producer_;
    std::vector<std::unique_ptr<KafkaConsumer>> consumers_;

    // Producer poll timer
    std::unique_ptr<boost::asio::steady_timer> producer_poll_timer_;
    bool draining_ = false;
};

} // namespace apex::shared::adapters::kafka
