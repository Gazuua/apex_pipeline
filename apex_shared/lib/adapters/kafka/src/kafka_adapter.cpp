#include <apex/shared/adapters/kafka/kafka_adapter.hpp>

#include <spdlog/spdlog.h>

#include <cassert>
#include <chrono>
#include <stdexcept>
#include <string>

namespace apex::shared::adapters::kafka {

KafkaAdapter::KafkaAdapter(KafkaConfig config)
    : config_(std::move(config))
    , producer_(std::make_unique<KafkaProducer>(config_)) {}

KafkaAdapter::~KafkaAdapter() {
    // Ensure cleanup even if close() was not explicitly called.
    // Cancel timer first to prevent use-after-free when io_context is destroyed.
    if (producer_poll_timer_) {
        producer_poll_timer_->cancel();
        producer_poll_timer_.reset();
    }
    // Stop consumers before their io_context references become invalid.
    for (auto& consumer : consumers_) {
        consumer->stop_consuming();
    }
    consumers_.clear();
    producer_.reset();
}

void KafkaAdapter::do_init(apex::core::CoreEngine& engine) {
    // 1. Initialize Producer
    auto result = producer_->init();
    if (!result.has_value()) {
        spdlog::error("KafkaAdapter: Producer init failed — aborting adapter init");
        throw std::runtime_error("KafkaAdapter: Producer init failed");
    }

    // 2. Create + initialize per-core Consumers
    uint32_t num_cores = engine.core_count();
    consumers_.reserve(num_cores);
    for (uint32_t i = 0; i < num_cores; ++i) {
        auto consumer = std::make_unique<KafkaConsumer>(
            config_, i, engine.io_context(i), producer_.get());
        if (message_cb_) {
            consumer->set_message_callback(message_cb_);
        }
        auto init_result = consumer->init();
        if (!init_result.has_value()) {
            spdlog::error("KafkaAdapter: Consumer[core={}] init failed — aborting adapter init", i);
            throw std::runtime_error(
                "KafkaAdapter: Consumer[core=" + std::to_string(i) + "] init failed");
        }
        consumers_.push_back(std::move(consumer));
    }

    // 3. Start consuming
    for (auto& consumer : consumers_) {
        if (consumer->initialized()) {
            consumer->start_consuming();
        }
    }

    // 4. Start Producer poll timer (on core 0's io_context)
    start_producer_poll_timer(engine.io_context(0));

    spdlog::info("KafkaAdapter initialized: {} cores, brokers={}",
                  num_cores, config_.brokers);
}

void KafkaAdapter::do_drain() {
    state_.store(AdapterState::DRAINING, std::memory_order_release);
    // Stop Consumer consumption
    for (auto& consumer : consumers_) {
        consumer->stop_consuming();
    }
    spdlog::info("KafkaAdapter: drain started");
}

void KafkaAdapter::do_close() {
    state_.store(AdapterState::CLOSED, std::memory_order_release);
    // Stop Producer poll timer
    if (producer_poll_timer_) {
        producer_poll_timer_->cancel();
    }

    // Flush Producer (max 10s wait)
    if (producer_->initialized()) {
        spdlog::info("KafkaAdapter: flushing producer (outq={})",
                      producer_->outq_len());
        producer_->flush(std::chrono::seconds{10});
    }

    // Clean up Consumers
    consumers_.clear();

    // Clean up Producer (destructor does flush + destroy)
    producer_.reset();

    spdlog::info("KafkaAdapter: closed");
}

apex::core::Result<void> KafkaAdapter::produce(
    std::string_view topic,
    std::string_view key,
    std::span<const uint8_t> payload)
{
    if (!is_running()) {
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    return producer_->produce(topic, key, payload);
}

apex::core::Result<void> KafkaAdapter::produce(
    std::string_view topic,
    std::string_view key,
    std::string_view payload)
{
    if (!is_running()) {
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    return producer_->produce(topic, key, payload);
}

void KafkaAdapter::set_message_callback(MessageCallback cb) {
    message_cb_ = cb;
    // Propagate to already-initialized consumers (for late callback registration)
    for (auto& consumer : consumers_) {
        consumer->set_message_callback(cb);
    }
}

KafkaConsumer& KafkaAdapter::consumer(uint32_t core_id) {
    assert(core_id < consumers_.size() && "core_id out of range");
    return *consumers_[core_id];
}

void KafkaAdapter::start_producer_poll_timer(boost::asio::io_context& io_ctx) {
    producer_poll_timer_ = std::make_unique<boost::asio::steady_timer>(io_ctx);
    on_producer_poll_tick();
}

void KafkaAdapter::on_producer_poll_tick() {
    if (!producer_poll_timer_ || !is_running()) return;

    // Process delivery callbacks
    producer_->poll(0);

    // Reschedule at 100ms interval (delivery report latency <= 100ms)
    producer_poll_timer_->expires_after(std::chrono::milliseconds{100});
    producer_poll_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec) return;
        on_producer_poll_tick();
    });
}

} // namespace apex::shared::adapters::kafka
