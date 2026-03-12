#include <apex/shared/adapters/redis/redis_pool.hpp>

namespace apex::shared::adapters::redis {

RedisPool::RedisPool(boost::asio::io_context& io_ctx,
                     const RedisConfig& config,
                     PoolConfig pool_config)
    : ConnectionPool(std::move(pool_config))
    , io_ctx_(io_ctx)
    , config_(config) {}

RedisPool::~RedisPool() = default;

std::unique_ptr<RedisConnection> RedisPool::do_create_connection() {
    return RedisConnection::create(io_ctx_, config_);
}

void RedisPool::do_destroy_connection(std::unique_ptr<RedisConnection>& conn) {
    if (conn) {
        conn->disconnect();
        conn.reset();
    }
}

bool RedisPool::do_validate(std::unique_ptr<RedisConnection>& conn) {
    return conn && conn->validate();
}

} // namespace apex::shared::adapters::redis
