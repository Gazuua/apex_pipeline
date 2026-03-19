#include <apex/core/error_code.hpp>
#include <apex/shared/adapters/pg/pg_config.hpp>
#include <apex/shared/adapters/pg/pg_pool.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <chrono>

using namespace apex::shared::adapters::pg;

class PgPoolRetryTest : public ::testing::Test
{
  protected:
    boost::asio::io_context io_;
};

template <typename Fn> void run_coro(boost::asio::io_context& io, Fn&& fn)
{
    io.restart();
    boost::asio::co_spawn(io, std::forward<Fn>(fn), boost::asio::detached);
    io.run();
}

// TC1: Pool has idle connection — immediate success (no retry needed)
TEST_F(PgPoolRetryTest, ImmediateSuccessNoRetry)
{
    PgAdapterConfig config{
        .pool_size_per_core = 2,
        .max_acquire_retries = 3,
        .retry_backoff = std::chrono::milliseconds{10},
    };
    PgPool pool(io_, config);

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto result = co_await pool.acquire_with_retry();
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(pool.active_count(), 1u);
        pool.release(std::move(result.value()));
        co_return;
    });
}

// TC2: Pool exhausted initially, release after short delay — retry succeeds
TEST_F(PgPoolRetryTest, RetryAfterExhaustion)
{
    PgAdapterConfig config{
        .pool_size_per_core = 1, // max = 2
        .max_acquire_retries = 3,
        .retry_backoff = std::chrono::milliseconds{50},
    };
    PgPool pool(io_, config);

    // Exhaust the pool
    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());

    // Schedule a release after a short delay
    boost::asio::steady_timer release_timer(io_, std::chrono::milliseconds{30});
    release_timer.async_wait([&](const boost::system::error_code& ec) {
        if (!ec)
        {
            pool.release(std::move(c1.value()));
        }
    });

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        // First acquire() will fail (pool exhausted),
        // but after backoff + release, next retry should succeed
        auto result = co_await pool.acquire_with_retry();
        EXPECT_TRUE(result.has_value());
        pool.release(std::move(result.value()));
        co_return;
    });

    pool.release(std::move(c2.value()));
}

// TC3: Pool exhausted + max_retries exceeded — returns PoolExhausted
TEST_F(PgPoolRetryTest, MaxRetriesExceededReturnsPoolExhausted)
{
    PgAdapterConfig config{
        .pool_size_per_core = 1, // max = 2
        .max_acquire_retries = 2,
        .retry_backoff = std::chrono::milliseconds{10},
    };
    PgPool pool(io_, config);

    // Exhaust the pool entirely — hold all connections
    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto result = co_await pool.acquire_with_retry();
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), apex::core::ErrorCode::PoolExhausted);
        co_return;
    });

    pool.release(std::move(c1.value()));
    pool.release(std::move(c2.value()));
}

// TC4: Exponential backoff timing — verify total elapsed time is reasonable
TEST_F(PgPoolRetryTest, ExponentialBackoffTiming)
{
    PgAdapterConfig config{
        .pool_size_per_core = 1, // max = 2
        .max_acquire_retries = 3,
        .retry_backoff = std::chrono::milliseconds{20},
    };
    PgPool pool(io_, config);

    // Exhaust the pool entirely
    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto start = std::chrono::steady_clock::now();
        auto result = co_await pool.acquire_with_retry();
        auto elapsed = std::chrono::steady_clock::now() - start;

        EXPECT_FALSE(result.has_value());
        // Expected backoff: 20ms + 40ms + 80ms = 140ms total
        // With timer overhead, allow range [100ms, 500ms]
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        EXPECT_GE(ms, 100) << "Too fast — backoff not applied";
        EXPECT_LE(ms, 500) << "Too slow — unexpected delays";
        co_return;
    });

    pool.release(std::move(c1.value()));
    pool.release(std::move(c2.value()));
}
