#include <apex/shared/adapters/redis/redis_adapter.hpp>

#include <spdlog/spdlog.h>

#include <cassert>

namespace apex::shared::adapters::redis {

RedisAdapter::RedisAdapter(RedisConfig config)
    : config_(std::move(config)) {}

RedisAdapter::~RedisAdapter() {
    // 안전 정리: close()가 호출되지 않은 경우에도 풀 소멸자가 커넥션 정리
    pools_.clear();
}

void RedisAdapter::do_init(apex::core::CoreEngine& engine) {
    pools_.reserve(engine.core_count());
    for (uint32_t i = 0; i < engine.core_count(); ++i) {
        PoolConfig pool_cfg{
            .min_size = config_.pool_size_per_core,
            .max_size = config_.pool_max_size_per_core,
            .max_idle_time = config_.max_idle_time,
            .health_check_interval = config_.health_check_interval,
        };
        pools_.push_back(std::make_unique<RedisPool>(
            engine.io_context(i), config_, pool_cfg));
    }

    spdlog::info("RedisAdapter initialized: {} cores, host={}:{}",
                  engine.core_count(), config_.host, config_.port);
}

void RedisAdapter::do_drain() {
    draining_ = true;
    spdlog::info("RedisAdapter: drain started");
}

void RedisAdapter::do_close() {
    // 모든 풀의 커넥션 정리
    for (auto& pool : pools_) {
        pool->close_all();
    }
    pools_.clear();
    spdlog::info("RedisAdapter: closed");
}

size_t RedisAdapter::active_connections() const noexcept {
    size_t total = 0;
    for (const auto& pool : pools_) {
        total += pool->active_count();
    }
    return total;
}

size_t RedisAdapter::idle_connections() const noexcept {
    size_t total = 0;
    for (const auto& pool : pools_) {
        total += pool->idle_count();
    }
    return total;
}

RedisPool& RedisAdapter::pool(uint32_t core_id) {
    assert(core_id < pools_.size() && "core_id out of range");
    return *pools_[core_id];
}

} // namespace apex::shared::adapters::redis
