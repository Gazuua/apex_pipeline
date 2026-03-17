#include <apex/shared/adapters/redis/redis_adapter.hpp>

#include <spdlog/spdlog.h>

#include <cassert>
#include <stdexcept>

namespace apex::shared::adapters::redis {

RedisAdapter::RedisAdapter(RedisConfig config)
    : config_(std::move(config)) {}

RedisAdapter::~RedisAdapter() {
    per_core_.clear();
}

void RedisAdapter::do_init(apex::core::CoreEngine& engine) {
    // Validate configuration before creating multiplexers
    if (config_.host.empty()) {
        spdlog::error("RedisAdapter: host is empty — aborting adapter init");
        throw std::runtime_error("RedisAdapter: host is empty");
    }
    if (config_.port == 0) {
        spdlog::error("RedisAdapter: port is 0 — aborting adapter init");
        throw std::runtime_error("RedisAdapter: port must be > 0");
    }

    per_core_.reserve(engine.core_count());
    for (uint32_t i = 0; i < engine.core_count(); ++i) {
        auto mux = std::make_unique<RedisMultiplexer>(
            engine.io_context(i), config_);
        mux->connect();
        per_core_.push_back(std::move(mux));
    }

    spdlog::info("RedisAdapter initialized: {} cores, host={}:{}",
                  engine.core_count(), config_.host, config_.port);
}

void RedisAdapter::do_drain() {
    spdlog::info("RedisAdapter: drain started");
}

void RedisAdapter::do_close() {
    per_core_.clear();
    spdlog::info("RedisAdapter: closed");
}

RedisMultiplexer& RedisAdapter::multiplexer(uint32_t core_id) {
    assert(core_id < per_core_.size() && "core_id out of range");
    return *per_core_[core_id];
}

std::size_t RedisAdapter::active_connections() const noexcept {
    std::size_t count = 0;
    for (const auto& mux : per_core_) {
        if (mux->connected()) ++count;
    }
    return count;
}

} // namespace apex::shared::adapters::redis
