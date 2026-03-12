#include <apex/shared/adapters/pg/pg_pool.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>
#include <apex/shared/adapters/connection_pool.hpp>

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters::pg;
using namespace apex::shared::adapters;

// =============================================================================
// PgPool configuration and pool sizing tests
// =============================================================================

TEST(PgPool, ConstructionWithConfig) {
    boost::asio::io_context io_ctx;
    PgAdapterConfig config{
        .connection_string = "host=localhost port=6432 dbname=test",
        .pool_size_per_core = 3,
        .max_idle_time = std::chrono::seconds{60},
        .health_check_interval = std::chrono::seconds{15},
    };

    PgPool pool(io_ctx, config);
    EXPECT_EQ(pool.config().min_size, 3u);       // pool_size_per_core
    EXPECT_EQ(pool.config().max_size, 6u);        // pool_size_per_core * 2
    EXPECT_EQ(pool.active_count(), 0u);
    EXPECT_EQ(pool.idle_count(), 0u);
    EXPECT_EQ(pool.total_count(), 0u);
}

TEST(PgPool, DefaultConfig) {
    boost::asio::io_context io_ctx;
    PgAdapterConfig config;

    PgPool pool(io_ctx, config);
    EXPECT_EQ(pool.config().min_size, 2u);    // default pool_size_per_core
    EXPECT_EQ(pool.config().max_size, 4u);    // default * 2
}

TEST(PgPool, ConnectionStringAccessible) {
    boost::asio::io_context io_ctx;
    PgAdapterConfig config{.connection_string = "host=db.example.com"};
    PgPool pool(io_ctx, config);
    EXPECT_EQ(pool.connection_string(), "host=db.example.com");
}

TEST(PgPool, AcquireCreatesUnconnectedConnection) {
    boost::asio::io_context io_ctx;
    PgAdapterConfig config{.pool_size_per_core = 2};
    PgPool pool(io_ctx, config);

    // acquire() is synchronous -- returns unconnected PgConnection
    auto result = pool.acquire();
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value()->is_connected());  // not yet connect_async'd
    EXPECT_EQ(pool.active_count(), 1u);
    EXPECT_EQ(pool.total_count(), 1u);

    pool.release(std::move(result.value()));
    EXPECT_EQ(pool.active_count(), 0u);
    EXPECT_EQ(pool.idle_count(), 1u);
}

TEST(PgPool, CloseAllCleansUp) {
    boost::asio::io_context io_ctx;
    PgAdapterConfig config{.pool_size_per_core = 2};
    PgPool pool(io_ctx, config);

    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());

    pool.release(std::move(c1.value()));
    pool.release(std::move(c2.value()));
    EXPECT_EQ(pool.idle_count(), 2u);

    pool.close_all();
    EXPECT_EQ(pool.idle_count(), 0u);
    EXPECT_EQ(pool.total_count(), 0u);
}

TEST(PgPool, ExhaustedPoolReturnsError) {
    boost::asio::io_context io_ctx;
    PgAdapterConfig config{.pool_size_per_core = 1};  // max = 2
    PgPool pool(io_ctx, config);

    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());

    // Pool exhausted
    auto c3 = pool.acquire();
    EXPECT_FALSE(c3.has_value());
    EXPECT_EQ(c3.error(), apex::core::ErrorCode::AdapterError);

    pool.release(std::move(c1.value()));
    pool.release(std::move(c2.value()));
}

TEST(PgPool, AcquireAfterRelease) {
    boost::asio::io_context io_ctx;
    PgAdapterConfig config{.pool_size_per_core = 1};  // max = 2
    PgPool pool(io_ctx, config);

    auto c1 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    EXPECT_EQ(pool.total_count(), 1u);

    pool.release(std::move(c1.value()));
    EXPECT_EQ(pool.idle_count(), 1u);

    // Re-acquire should reuse idle connection (but unconnected, so validate returns false,
    // so it creates new). Both paths are fine.
    auto c2 = pool.acquire();
    ASSERT_TRUE(c2.has_value());
    EXPECT_EQ(pool.active_count(), 1u);
}

TEST(PgPool, MultipleAcquireWithinMax) {
    boost::asio::io_context io_ctx;
    PgAdapterConfig config{.pool_size_per_core = 2};  // max = 4
    PgPool pool(io_ctx, config);

    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    auto c3 = pool.acquire();
    auto c4 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());
    ASSERT_TRUE(c3.has_value());
    ASSERT_TRUE(c4.has_value());
    EXPECT_EQ(pool.active_count(), 4u);

    // max_size=4 exceeded
    auto c5 = pool.acquire();
    EXPECT_FALSE(c5.has_value());

    pool.release(std::move(c1.value()));
    pool.release(std::move(c2.value()));
    pool.release(std::move(c3.value()));
    pool.release(std::move(c4.value()));
}
