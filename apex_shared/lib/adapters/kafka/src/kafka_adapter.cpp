#include <apex/core/service_base.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>
#include <apex/shared/protocols/kafka/kafka_dispatch_bridge.hpp>

#include <spdlog/spdlog.h>

#include <cassert>
#include <chrono>
#include <stdexcept>
#include <string>

namespace apex::shared::adapters::kafka
{

KafkaAdapter::KafkaAdapter(KafkaConfig config)
    : config_(std::move(config))
    , payload_pool_(config_.payload_pool_initial_count, config_.payload_pool_buffer_size,
                    config_.payload_pool_max_count)
    , producer_(std::make_unique<KafkaProducer>(config_))
{}

KafkaAdapter::~KafkaAdapter()
{
    // Ensure cleanup even if close() was not explicitly called.
    // Cancel timer first to prevent use-after-free when io_context is destroyed.
    if (producer_poll_timer_)
    {
        producer_poll_timer_->cancel();
        producer_poll_timer_.reset();
    }
    // Stop consumers before their io_context references become invalid.
    for (auto& consumer : consumers_)
    {
        consumer->stop_consuming();
    }
    consumers_.clear();
    producer_.reset();
}

void KafkaAdapter::do_init(apex::core::CoreEngine& engine)
{
    // 1. Initialize Producer
    auto result = producer_->init();
    if (!result.has_value())
    {
        spdlog::error("KafkaAdapter: Producer init failed — aborting adapter init");
        throw std::runtime_error("KafkaAdapter: Producer init failed");
    }

    // 2. Create + initialize per-core Consumers
    uint32_t num_cores = engine.core_count();
    consumers_.reserve(num_cores);
    for (uint32_t i = 0; i < num_cores; ++i)
    {
        auto consumer = std::make_unique<KafkaConsumer>(config_, i, engine.io_context(i), producer_.get());
        if (message_cb_)
        {
            consumer->set_message_callback(message_cb_);
        }
        auto init_result = consumer->init();
        if (!init_result.has_value())
        {
            spdlog::error("KafkaAdapter: Consumer[core={}] init failed — aborting adapter init", i);
            throw std::runtime_error("KafkaAdapter: Consumer[core=" + std::to_string(i) + "] init failed");
        }
        consumers_.push_back(std::move(consumer));
    }

    // 3. Start consuming
    for (auto& consumer : consumers_)
    {
        if (consumer->initialized())
        {
            consumer->start_consuming();
        }
    }

    // 4. Start Producer poll timer (on core 0's io_context)
    start_producer_poll_timer(engine.io_context(0));

    spdlog::info("KafkaAdapter initialized: {} cores, brokers={}, payload_pool(init={}, buf={}B, max={})", num_cores,
                 config_.brokers, config_.payload_pool_initial_count, config_.payload_pool_buffer_size,
                 config_.payload_pool_max_count);
}

void KafkaAdapter::do_drain()
{
    state_.store(AdapterState::DRAINING, std::memory_order_release);
    // Stop Consumer consumption
    for (auto& consumer : consumers_)
    {
        consumer->stop_consuming();
    }
    spdlog::info("KafkaAdapter: drain started");
}

void KafkaAdapter::do_close()
{
    state_.store(AdapterState::CLOSED, std::memory_order_release);
    // Stop Producer poll timer
    if (producer_poll_timer_)
    {
        producer_poll_timer_->cancel();
    }

    // Flush Producer
    if (producer_->initialized())
    {
        spdlog::info("KafkaAdapter: flushing producer (outq={})", producer_->outq_len());
        auto flushed = producer_->flush(std::chrono::milliseconds{config_.flush_timeout_ms});
        if (!flushed)
        {
            spdlog::warn("KafkaAdapter: producer flush timed out ({}ms)", config_.flush_timeout_ms);
        }
    }

    // Clean up Consumers
    consumers_.clear();

    // Clean up Producer (destructor does flush + destroy)
    producer_.reset();

    // Payload pool metrics
    spdlog::info("KafkaAdapter: payload_pool stats — acquired={}, fallback={}, peak_in_use={}, free={}",
                 payload_pool_.acquire_count(), payload_pool_.fallback_alloc_count(), payload_pool_.peak_in_use(),
                 payload_pool_.free_count());

    spdlog::info("KafkaAdapter: closed");
}

apex::core::Result<void> KafkaAdapter::produce(std::string_view topic, std::string_view key,
                                               std::span<const uint8_t> payload)
{
    if (!is_running())
    {
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    return producer_->produce(topic, key, payload);
}

apex::core::Result<void> KafkaAdapter::produce(std::string_view topic, std::string_view key, std::string_view payload)
{
    if (!is_running())
    {
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    return producer_->produce(topic, key, payload);
}

void KafkaAdapter::wire_services(std::vector<std::unique_ptr<apex::core::ServiceBaseInterface>>& services,
                                 apex::core::CoreEngine& engine)
{
    for (auto& svc : services)
    {
        if (!svc->has_kafka_handlers())
            continue;

        auto bridge = std::make_shared<apex::shared::protocols::kafka::KafkaDispatchBridge>(svc->kafka_handler_map());

        // request_topic: consume_topics의 첫 번째 토픽을 request_topic으로 사용.
        // 서비스별 TOML에서 consume_topics에 request_topic을 설정하므로 일관됨.
        std::string request_topic;
        if (!config_.consume_topics.empty())
        {
            request_topic = config_.consume_topics[0];
        }

        // 기존 Auth/Chat main.cpp의 패턴을 자동화:
        // consumer callback에서 topic 필터링 후 bridge->dispatch() 호출
        set_message_callback([bridge, request_topic, &engine,
                              this](std::string_view topic, int32_t /*partition*/, std::span<const uint8_t> /*key*/,
                                    std::span<const uint8_t> payload, int64_t /*offset*/) -> apex::core::Result<void> {
            if (!request_topic.empty() && topic != request_topic)
            {
                return apex::core::ok(); // 다른 토픽 무시
            }

            // Kafka 콜백의 payload는 콜백 반환 후 무효 → 풀에서 버퍼 획득 후 복사
            auto pooled_buf = payload_pool_.acquire(payload);

            // spawn_tracked로 코루틴 실행 — bridge->dispatch가 파싱+핸들러 호출 수행
            engine.spawn_tracked(0, [bridge, buf = std::move(pooled_buf)]() -> boost::asio::awaitable<void> {
                auto result = co_await bridge->dispatch(buf->span());
                if (!result.has_value())
                {
                    spdlog::warn("[KafkaAdapter] auto-wired dispatch failed: {}", static_cast<int>(result.error()));
                }
            });

            return apex::core::ok();
        });

        spdlog::info("[KafkaAdapter] Auto-wired KafkaDispatchBridge for service '{}'", svc->name());
        break; // 현재 1 adapter = 1 service 패턴 (multi-service는 v0.6+)
    }
}

void KafkaAdapter::set_message_callback(MessageCallback cb)
{
    message_cb_ = cb;
    // Propagate to already-initialized consumers (for late callback registration)
    for (auto& consumer : consumers_)
    {
        consumer->set_message_callback(cb);
    }
}

KafkaConsumer& KafkaAdapter::consumer(uint32_t core_id)
{
    assert(core_id < consumers_.size() && "core_id out of range");
    return *consumers_[core_id];
}

void KafkaAdapter::start_producer_poll_timer(boost::asio::io_context& io_ctx)
{
    producer_poll_timer_ = std::make_unique<boost::asio::steady_timer>(io_ctx);
    on_producer_poll_tick();
}

void KafkaAdapter::on_producer_poll_tick()
{
    if (!producer_poll_timer_ || !is_running())
        return;

    // Process delivery callbacks
    producer_->poll(0);

    // Reschedule at configured interval (delivery report latency bound)
    producer_poll_timer_->expires_after(std::chrono::milliseconds{config_.producer_poll_interval_ms});
    producer_poll_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec)
            return;
        on_producer_poll_tick();
    });
}

} // namespace apex::shared::adapters::kafka
