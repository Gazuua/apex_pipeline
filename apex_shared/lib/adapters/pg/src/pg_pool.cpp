#include <apex/shared/adapters/pg/pg_pool.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>

#include <boost/asio/use_awaitable.hpp>

#include <utility>

namespace apex::shared::adapters::pg {

PgPool::PgPool(boost::asio::io_context& io_ctx, const PgAdapterConfig& config)
    : io_ctx_(io_ctx)
    , config_(config)
    , pool_config_{
          .min_size = config.pool_size_per_core,
          .max_size = config.pool_size_per_core * 2,  // auto-grow headroom
          .max_idle_time = config.max_idle_time,
          .health_check_interval = config.health_check_interval,
      } {}

PgPool::~PgPool() { close_all(); }

// --- PoolLike contract ---

apex::core::Result<PgPool::Connection> PgPool::acquire() {
    // Try idle queue first
    while (!idle_.empty()) {
        auto entry = std::move(idle_.front());
        idle_.pop_front();
        if (do_validate(entry.conn)) {
            ++active_count_;
            ++stats_.total_acquired;
            return std::move(entry.conn);
        }
        do_destroy_connection(entry.conn);
        --total_count_;
        ++stats_.total_destroyed;
    }
    // Create new connection if within limit
    if (total_count_ < pool_config_.max_size) {
        auto conn = do_create_connection();
        if (!conn) {
            ++stats_.total_failed;
            return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
        ++total_count_;
        ++active_count_;
        ++stats_.total_created;
        ++stats_.total_acquired;
        return std::move(conn);
    }
    ++stats_.total_failed;
    return std::unexpected(apex::core::ErrorCode::AdapterError);
}

void PgPool::release(Connection conn) {
    if (conn && conn->is_poisoned()) {
        discard(std::move(conn));
        return;
    }
    --active_count_;
    ++stats_.total_released;
    idle_.push_back({std::move(conn),
                     std::chrono::steady_clock::now()});
}

void PgPool::discard(Connection conn) {
    do_destroy_connection(conn);
    --active_count_;
    --total_count_;
    ++stats_.total_destroyed;
}

void PgPool::shrink_idle() {
    auto now = std::chrono::steady_clock::now();
    while (!idle_.empty()) {
        auto& front = idle_.front();
        if (now - front.returned_at < pool_config_.max_idle_time) break;
        do_destroy_connection(front.conn);
        idle_.pop_front();
        --total_count_;
        ++stats_.total_destroyed;
    }
}

void PgPool::health_check_tick() {
    std::size_t checked = 0;
    while (!idle_.empty() && checked < idle_.size()) {
        auto entry = std::move(idle_.front());
        idle_.pop_front();
        if (do_validate(entry.conn)) {
            idle_.push_back({std::move(entry.conn),
                             std::chrono::steady_clock::now()});
        } else {
            do_destroy_connection(entry.conn);
            --total_count_;
            ++stats_.total_destroyed;
        }
        ++checked;
    }
}

void PgPool::close_all() {
    while (!idle_.empty()) {
        auto entry = std::move(idle_.front());
        idle_.pop_front();
        do_destroy_connection(entry.conn);
        --total_count_;
        ++stats_.total_destroyed;
    }
}

PoolStats PgPool::stats() const noexcept { return stats_; }

std::size_t PgPool::active_count() const noexcept { return active_count_; }
std::size_t PgPool::idle_count() const noexcept { return idle_.size(); }
std::size_t PgPool::total_count() const noexcept { return total_count_; }
const PoolConfig& PgPool::config() const noexcept { return pool_config_; }

// --- PG-specific hooks ---

PgPool::Connection PgPool::do_create_connection() {
    return std::make_unique<PgConnection>(io_ctx_);
}

void PgPool::do_destroy_connection(Connection& conn) {
    if (conn) {
        conn->close();
        conn.reset();
    }
}

bool PgPool::do_validate(Connection& conn) {
    return conn && conn->is_valid();
}

// --- PG-specific async API ---

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
        discard(std::move(conn));
        co_return std::unexpected(connect_result.error());
    }
    co_return std::move(conn);
}

const std::string& PgPool::connection_string() const noexcept {
    return config_.connection_string;
}

} // namespace apex::shared::adapters::pg
