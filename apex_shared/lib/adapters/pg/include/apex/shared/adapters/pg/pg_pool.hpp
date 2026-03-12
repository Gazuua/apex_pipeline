#pragma once

#include <apex/shared/adapters/pg/pg_config.hpp>
#include <apex/shared/adapters/pg/pg_connection.hpp>
#include <apex/shared/adapters/connection_pool.hpp>
#include <apex/core/result.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <string_view>

namespace apex::shared::adapters::pg {

/// Per-core PostgreSQL connection pool.
/// Inherits ConnectionPool CRTP for acquire/release/health_check logic reuse.
///
/// Pool size is small (default 2/core) because PgBouncer handles server-side pooling.
///
/// Design: do_create_connection() returns an unconnected PgConnection.
/// acquire_connected() performs connect_async() lazily on first use.
/// This preserves ConnectionPool's synchronous interface while supporting
/// async connections.
class PgPool : public ConnectionPool<PgPool, std::unique_ptr<PgConnection>> {
public:
    PgPool(boost::asio::io_context& io_ctx, const PgAdapterConfig& config);
    ~PgPool();

    // Non-copyable, non-movable
    PgPool(const PgPool&) = delete;
    PgPool& operator=(const PgPool&) = delete;

    // --- ConnectionPool CRTP requirements ---
    std::unique_ptr<PgConnection> do_create_connection();
    void do_destroy_connection(std::unique_ptr<PgConnection>& conn);
    bool do_validate(std::unique_ptr<PgConnection>& conn);

    /// Async connection creation + connect (for pool warm-up)
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<std::unique_ptr<PgConnection>>>
    create_connected();

    /// Acquire connection from pool, connecting if needed.
    /// This is the primary usage path.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<std::unique_ptr<PgConnection>>>
    acquire_connected();

    /// Connection string access (debug/logging)
    [[nodiscard]] std::string_view connection_string() const noexcept;

private:
    boost::asio::io_context& io_ctx_;
    PgAdapterConfig config_;
};

} // namespace apex::shared::adapters::pg
