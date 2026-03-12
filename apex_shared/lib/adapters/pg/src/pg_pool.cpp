#include <apex/shared/adapters/pg/pg_pool.hpp>

#include <boost/asio/use_awaitable.hpp>

#include <utility>

namespace apex::shared::adapters::pg {

PgPool::PgPool(boost::asio::io_context& io_ctx, const PgAdapterConfig& config)
    : ConnectionPool(PoolConfig{
          .min_size = config.pool_size_per_core,
          .max_size = config.pool_size_per_core * 2,  // auto-grow headroom
          .max_idle_time = config.max_idle_time,
          .health_check_interval = config.health_check_interval,
      })
    , io_ctx_(io_ctx)
    , config_(config) {}

PgPool::~PgPool() = default;

std::unique_ptr<PgConnection> PgPool::do_create_connection() {
    // Create unconnected PgConnection (io_context binding only).
    // Actual connection is established via connect_async() in acquire_connected().
    return std::make_unique<PgConnection>(io_ctx_);
}

void PgPool::do_destroy_connection(std::unique_ptr<PgConnection>& conn) {
    if (conn) {
        conn->close();
        conn.reset();
    }
}

bool PgPool::do_validate(std::unique_ptr<PgConnection>& conn) {
    return conn && conn->is_valid();
}

boost::asio::awaitable<apex::core::Result<std::unique_ptr<PgConnection>>>
PgPool::create_connected() {
    auto conn = std::make_unique<PgConnection>(io_ctx_);
    auto result = co_await conn->connect_async(config_.connection_string);
    if (!result.has_value()) {
        co_return std::unexpected(result.error());
    }
    co_return std::move(conn);
}

boost::asio::awaitable<apex::core::Result<std::unique_ptr<PgConnection>>>
PgPool::acquire_connected() {
    auto result = acquire();
    if (!result.has_value()) {
        co_return std::unexpected(result.error());
    }
    auto conn = std::move(result.value());

    // Already connected -> return immediately
    if (conn->is_connected()) {
        co_return std::move(conn);
    }

    // Not yet connected -> establish connection
    auto connect_result = co_await conn->connect_async(config_.connection_string);
    if (!connect_result.has_value()) {
        // Connection failed -- destroy, don't return to pool
        co_return std::unexpected(connect_result.error());
    }
    co_return std::move(conn);
}

std::string_view PgPool::connection_string() const noexcept {
    return config_.connection_string;
}

} // namespace apex::shared::adapters::pg
