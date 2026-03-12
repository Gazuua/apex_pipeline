#pragma once

#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/adapters/redis/redis_connection.hpp>
#include <apex/shared/adapters/connection_pool.hpp>
#include <apex/core/result.hpp>

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace apex::shared::adapters::redis {

/// 코어별 독립 Redis 커넥션 풀.
/// ConnectionPool CRTP를 상속하여 acquire/release/shrink/health_check 로직을 재사용.
/// 각 커넥션은 해당 코어의 io_context에 바인딩된 hiredis 비동기 커넥션.
class RedisPool : public ConnectionPool<RedisPool, std::unique_ptr<RedisConnection>> {
public:
    RedisPool(boost::asio::io_context& io_ctx,
              const RedisConfig& config,
              PoolConfig pool_config);
    ~RedisPool();

    // Non-copyable, non-movable
    RedisPool(const RedisPool&) = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    // --- ConnectionPool CRTP 구현 ---
    std::unique_ptr<RedisConnection> do_create_connection();
    void do_destroy_connection(std::unique_ptr<RedisConnection>& conn);
    bool do_validate(std::unique_ptr<RedisConnection>& conn);

    /// 설정 접근
    [[nodiscard]] const RedisConfig& redis_config() const noexcept { return config_; }

private:
    boost::asio::io_context& io_ctx_;
    RedisConfig config_;
};

} // namespace apex::shared::adapters::redis
