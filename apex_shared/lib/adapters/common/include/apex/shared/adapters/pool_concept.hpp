// apex_shared/lib/adapters/common/include/apex/shared/adapters/pool_concept.hpp
#pragma once

#include <apex/core/result.hpp>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>

namespace apex::shared::adapters {

/// 풀 통계 (기존 ConnectionPool::PoolStats 그대로)
struct PoolStats {
    uint64_t total_acquired = 0;
    uint64_t total_released = 0;
    uint64_t total_created = 0;
    uint64_t total_destroyed = 0;
    uint64_t total_failed = 0;
};

/// 풀 설정 (기존 ConnectionPool::PoolConfig 기본값 유지)
struct PoolConfig {
    std::size_t min_size = 1;
    std::size_t max_size = 8;
    std::chrono::seconds max_idle_time{60};
    std::chrono::seconds health_check_interval{30};
};

/// 동기 풀 기본 계약.
/// PgPool::acquire_connected() 같은 비동기 확장은 고유 API이며 범위 밖.
template <typename T>
concept PoolLike = requires(T pool) {
    typename T::Connection;
    { pool.acquire() }
        -> std::same_as<apex::core::Result<typename T::Connection>>;
    { pool.release(std::declval<typename T::Connection>()) }
        -> std::same_as<void>;
    { pool.discard(std::declval<typename T::Connection>()) }
        -> std::same_as<void>;
    { pool.close_all() } -> std::same_as<void>;
    { pool.stats() }     -> std::convertible_to<PoolStats>;
};

} // namespace apex::shared::adapters
