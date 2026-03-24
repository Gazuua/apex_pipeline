// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/metrics_registry.hpp>
#include <apex/shared/adapters/redis/redis_adapter.hpp>

#include <cassert>
#include <stdexcept>
#include <string>

namespace apex::shared::adapters::redis
{

RedisAdapter::RedisAdapter(RedisConfig config)
    : config_(std::move(config))
{}

RedisAdapter::~RedisAdapter()
{
    per_core_.clear();
}

void RedisAdapter::do_init(apex::core::CoreEngine& engine)
{
    // Validate configuration before creating multiplexers
    if (config_.host.empty())
    {
        logger_.error("host is empty — aborting adapter init");
        throw std::runtime_error("RedisAdapter: host is empty");
    }
    if (config_.port == 0)
    {
        logger_.error("port is 0 — aborting adapter init");
        throw std::runtime_error("RedisAdapter: port must be > 0");
    }

    per_core_.reserve(engine.core_count());
    for (uint32_t i = 0; i < engine.core_count(); ++i)
    {
        auto mux = std::make_unique<RedisMultiplexer>(engine.io_context(i), config_, i,
                                                      [this](uint32_t core_id, boost::asio::awaitable<void> coro) {
                                                          this->spawn_adapter_coro(core_id, std::move(coro));
                                                      });
        mux->connect();
        per_core_.push_back(std::move(mux));
    }

    logger_.info("initialized: {} cores, host={}:{}", engine.core_count(), config_.host, config_.port);
}

void RedisAdapter::do_drain()
{
    logger_.info("drain started");
}

void RedisAdapter::do_close()
{
    per_core_.clear();
    logger_.info("closed");
}

void RedisAdapter::do_close_per_core(uint32_t core_id)
{
    if (core_id < per_core_.size() && per_core_[core_id])
        per_core_[core_id]->close();
}

void RedisAdapter::do_register_metrics(apex::core::MetricsRegistry& registry)
{
    for (uint32_t i = 0; i < per_core_.size(); ++i)
    {
        auto labels = apex::core::Labels{{"core", std::to_string(i)}};
        registry.counter_from("apex_redis_commands_total", "Total Redis commands executed", labels,
                              per_core_[i]->metric_commands_total());

        auto* mux = per_core_[i].get();
        registry.gauge_fn("apex_redis_connected", "Whether Redis connection is established", labels,
                          [mux]() -> int64_t { return mux->connected() ? 1 : 0; });
    }
}

RedisMultiplexer& RedisAdapter::multiplexer(uint32_t core_id)
{
    assert(core_id < per_core_.size() && "core_id out of range");
    return *per_core_[core_id];
}

std::size_t RedisAdapter::active_connections() const noexcept
{
    std::size_t count = 0;
    for (const auto& mux : per_core_)
    {
        if (mux->connected())
            ++count;
    }
    return count;
}

} // namespace apex::shared::adapters::redis
