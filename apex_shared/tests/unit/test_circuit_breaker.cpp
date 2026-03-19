#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>
#include <apex/shared/adapters/circuit_breaker.hpp>

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

using namespace apex::shared::adapters;
using apex::core::ErrorCode;
using apex::core::Result;

class CircuitBreakerTest : public ::testing::Test
{
  protected:
    boost::asio::io_context io_;
};

// Helper: run a coroutine on io_context and block until done
template <typename Fn> void run_coro(boost::asio::io_context& io, Fn&& fn)
{
    io.restart();
    boost::asio::co_spawn(io, std::forward<Fn>(fn), boost::asio::detached);
    io.run();
}

// TC1: CLOSED state — successful call keeps CLOSED
TEST_F(CircuitBreakerTest, ClosedStateSuccessStaysClosed)
{
    CircuitBreaker cb(CircuitBreakerConfig{.failure_threshold = 3});

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto ok = [&]() -> boost::asio::awaitable<Result<void>> { co_return Result<void>{}; };
        auto result = co_await cb.call(ok);
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(cb.state(), CircuitState::CLOSED);
        EXPECT_EQ(cb.failure_count(), 0u);
        co_return;
    });
}

// TC2: CLOSED -> OPEN transition (failure_threshold reached)
TEST_F(CircuitBreakerTest, ClosedToOpenTransition)
{
    CircuitBreaker cb(CircuitBreakerConfig{.failure_threshold = 3});

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto fail = [&]() -> boost::asio::awaitable<Result<void>> {
            co_return std::unexpected(ErrorCode::AdapterError);
        };
        for (int i = 0; i < 3; ++i)
        {
            co_await cb.call(fail);
        }
        EXPECT_EQ(cb.state(), CircuitState::OPEN);
        EXPECT_EQ(cb.failure_count(), 3u);
        co_return;
    });
}

// TC3: OPEN state — call rejected with CircuitOpen
TEST_F(CircuitBreakerTest, OpenStateRejectsCall)
{
    CircuitBreaker cb(CircuitBreakerConfig{
        .failure_threshold = 2, .open_duration = std::chrono::hours{1} // long duration to stay OPEN
    });

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto fail = [&]() -> boost::asio::awaitable<Result<void>> {
            co_return std::unexpected(ErrorCode::AdapterError);
        };
        // Trip to OPEN
        for (int i = 0; i < 2; ++i)
            co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::OPEN);

        // Now subsequent call should be rejected without invoking fn
        auto ok = [&]() -> boost::asio::awaitable<Result<void>> {
            ADD_FAILURE() << "Should not be invoked in OPEN state";
            co_return Result<void>{};
        };
        auto result = co_await cb.call(ok);
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), ErrorCode::CircuitOpen);
        co_return;
    });
}

// TC4: OPEN -> HALF_OPEN transition (open_duration elapsed)
TEST_F(CircuitBreakerTest, OpenToHalfOpenTransition)
{
    CircuitBreaker cb(CircuitBreakerConfig{.failure_threshold = 2,
                                           .open_duration = std::chrono::milliseconds{1}, // very short
                                           .half_open_max_calls = 2});

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto fail = [&]() -> boost::asio::awaitable<Result<void>> {
            co_return std::unexpected(ErrorCode::AdapterError);
        };
        // Trip to OPEN
        for (int i = 0; i < 2; ++i)
            co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::OPEN);

        // Wait for open_duration to elapse
        boost::asio::steady_timer timer(io_, std::chrono::milliseconds{5});
        co_await timer.async_wait(boost::asio::use_awaitable);

        // Next call should transition to HALF_OPEN and be allowed
        auto ok = [&]() -> boost::asio::awaitable<Result<void>> { co_return Result<void>{}; };
        auto result = co_await cb.call(ok);
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);
        co_return;
    });
}

// TC5: HALF_OPEN -> CLOSED transition (half_open_max_calls successes)
TEST_F(CircuitBreakerTest, HalfOpenToClosedTransition)
{
    CircuitBreaker cb(CircuitBreakerConfig{
        .failure_threshold = 2, .open_duration = std::chrono::milliseconds{1}, .half_open_max_calls = 2});

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto fail = [&]() -> boost::asio::awaitable<Result<void>> {
            co_return std::unexpected(ErrorCode::AdapterError);
        };
        auto ok = [&]() -> boost::asio::awaitable<Result<void>> { co_return Result<void>{}; };

        // Trip to OPEN
        for (int i = 0; i < 2; ++i)
            co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::OPEN);

        // Wait for transition
        boost::asio::steady_timer timer(io_, std::chrono::milliseconds{5});
        co_await timer.async_wait(boost::asio::use_awaitable);

        // half_open_max_calls = 2 successes should close the circuit
        co_await cb.call(ok);
        EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);
        co_await cb.call(ok);
        EXPECT_EQ(cb.state(), CircuitState::CLOSED);
        EXPECT_EQ(cb.failure_count(), 0u);
        co_return;
    });
}

// TC6: HALF_OPEN -> OPEN transition (failure during half-open)
TEST_F(CircuitBreakerTest, HalfOpenToOpenOnFailure)
{
    CircuitBreaker cb(CircuitBreakerConfig{
        .failure_threshold = 2, .open_duration = std::chrono::milliseconds{1}, .half_open_max_calls = 3});

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto fail = [&]() -> boost::asio::awaitable<Result<void>> {
            co_return std::unexpected(ErrorCode::AdapterError);
        };
        auto ok = [&]() -> boost::asio::awaitable<Result<void>> { co_return Result<void>{}; };

        // Trip to OPEN
        for (int i = 0; i < 2; ++i)
            co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::OPEN);

        // Wait for transition to HALF_OPEN
        boost::asio::steady_timer timer(io_, std::chrono::milliseconds{5});
        co_await timer.async_wait(boost::asio::use_awaitable);

        // One success
        co_await cb.call(ok);
        EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);

        // Failure in HALF_OPEN -> back to OPEN
        co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::OPEN);
        co_return;
    });
}

// TC7: CLOSED state — partial failures reset on success (consecutive failure counting)
TEST_F(CircuitBreakerTest, ClosedPartialFailuresResetOnSuccess)
{
    CircuitBreaker cb(CircuitBreakerConfig{.failure_threshold = 3});

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto fail = [&]() -> boost::asio::awaitable<Result<void>> {
            co_return std::unexpected(ErrorCode::AdapterError);
        };
        auto ok = [&]() -> boost::asio::awaitable<Result<void>> { co_return Result<void>{}; };

        // Accumulate threshold-1 failures
        for (int i = 0; i < 2; ++i)
            co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::CLOSED);
        EXPECT_EQ(cb.failure_count(), 2u);

        // One success should reset failure_count to 0
        co_await cb.call(ok);
        EXPECT_EQ(cb.state(), CircuitState::CLOSED);
        EXPECT_EQ(cb.failure_count(), 0u);

        // Need full threshold again to trip
        for (int i = 0; i < 2; ++i)
            co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::CLOSED); // still CLOSED (only 2/3)
        co_return;
    });
}

// TC8: reset() test
TEST_F(CircuitBreakerTest, ResetClearsState)
{
    CircuitBreaker cb(CircuitBreakerConfig{.failure_threshold = 2});

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto fail = [&]() -> boost::asio::awaitable<Result<void>> {
            co_return std::unexpected(ErrorCode::AdapterError);
        };
        // Trip to OPEN
        for (int i = 0; i < 2; ++i)
            co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::OPEN);

        // Reset
        cb.reset();
        EXPECT_EQ(cb.state(), CircuitState::CLOSED);
        EXPECT_EQ(cb.failure_count(), 0u);

        // Should work normally now
        auto ok = [&]() -> boost::asio::awaitable<Result<void>> { co_return Result<void>{}; };
        auto result = co_await cb.call(ok);
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(cb.state(), CircuitState::CLOSED);
        co_return;
    });
}
