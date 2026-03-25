// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>
#include <apex/shared/adapters/circuit_breaker.hpp>

#include "../test_helpers.hpp"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>

using namespace apex::shared::adapters;
using apex::core::ErrorCode;
using apex::core::Result;
using apex::test::run_coro;

class CircuitBreakerTest : public ::testing::Test
{
  protected:
    boost::asio::io_context io_;
};

// TC1: CLOSED state — successful call keeps CLOSED
TEST_F(CircuitBreakerTest, ClosedStateSuccessStaysClosed)
{
    CircuitBreaker cb(CircuitBreakerConfig{
        .failure_threshold = 3, .open_duration = std::chrono::milliseconds{30000}, .half_open_max_calls = 3});

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
    CircuitBreaker cb(CircuitBreakerConfig{
        .failure_threshold = 3, .open_duration = std::chrono::milliseconds{30000}, .half_open_max_calls = 3});

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto fail = [&]() -> boost::asio::awaitable<Result<void>> {
            co_return std::unexpected(ErrorCode::AdapterError);
        };
        for (int i = 0; i < 3; ++i)
        {
            (void)co_await cb.call(fail);
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
        .failure_threshold = 2,
        .open_duration = std::chrono::hours{1}, // long duration to stay OPEN
        .half_open_max_calls = 3,
    });

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto fail = [&]() -> boost::asio::awaitable<Result<void>> {
            co_return std::unexpected(ErrorCode::AdapterError);
        };
        // Trip to OPEN
        for (int i = 0; i < 2; ++i)
            (void)co_await cb.call(fail);
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
            (void)co_await cb.call(fail);
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
            (void)co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::OPEN);

        // Wait for transition
        boost::asio::steady_timer timer(io_, std::chrono::milliseconds{5});
        co_await timer.async_wait(boost::asio::use_awaitable);

        // half_open_max_calls = 2 successes should close the circuit
        (void)co_await cb.call(ok);
        EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);
        EXPECT_EQ(cb.half_open_successes(), 1u);
        (void)co_await cb.call(ok);
        EXPECT_EQ(cb.state(), CircuitState::CLOSED);
        EXPECT_EQ(cb.failure_count(), 0u);
        EXPECT_EQ(cb.half_open_successes(), 0u); // CLOSED 전이 시 리셋
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
            (void)co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::OPEN);

        // Wait for transition to HALF_OPEN
        boost::asio::steady_timer timer(io_, std::chrono::milliseconds{5});
        co_await timer.async_wait(boost::asio::use_awaitable);

        // One success
        (void)co_await cb.call(ok);
        EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);

        // Failure in HALF_OPEN -> back to OPEN
        (void)co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::OPEN);
        co_return;
    });
}

// TC7: CLOSED state — partial failures reset on success (consecutive failure counting)
TEST_F(CircuitBreakerTest, ClosedPartialFailuresResetOnSuccess)
{
    CircuitBreaker cb(CircuitBreakerConfig{
        .failure_threshold = 3, .open_duration = std::chrono::milliseconds{30000}, .half_open_max_calls = 3});

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto fail = [&]() -> boost::asio::awaitable<Result<void>> {
            co_return std::unexpected(ErrorCode::AdapterError);
        };
        auto ok = [&]() -> boost::asio::awaitable<Result<void>> { co_return Result<void>{}; };

        // Accumulate threshold-1 failures
        for (int i = 0; i < 2; ++i)
            (void)co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::CLOSED);
        EXPECT_EQ(cb.failure_count(), 2u);

        // One success should reset failure_count to 0
        (void)co_await cb.call(ok);
        EXPECT_EQ(cb.state(), CircuitState::CLOSED);
        EXPECT_EQ(cb.failure_count(), 0u);

        // Need full threshold again to trip
        for (int i = 0; i < 2; ++i)
            (void)co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::CLOSED); // still CLOSED (only 2/3)
        co_return;
    });
}

// TC9: HALF_OPEN 성공 후 실패 — 성공 카운트가 OPEN 전이 시 소멸 확인 (BACKLOG-120)
TEST_F(CircuitBreakerTest, HalfOpenSuccessCountResetOnFailure)
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
            (void)co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::OPEN);

        // Wait for HALF_OPEN
        boost::asio::steady_timer timer(io_, std::chrono::milliseconds{5});
        co_await timer.async_wait(boost::asio::use_awaitable);

        // 2 successes (out of 3 needed)
        (void)co_await cb.call(ok);
        EXPECT_EQ(cb.half_open_successes(), 1u);
        (void)co_await cb.call(ok);
        EXPECT_EQ(cb.half_open_successes(), 2u);
        EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);

        // Failure sends back to OPEN — successes are lost
        (void)co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::OPEN);

        // Re-enter HALF_OPEN — successes counter starts from 0
        boost::asio::steady_timer timer2(io_, std::chrono::milliseconds{5});
        co_await timer2.async_wait(boost::asio::use_awaitable);

        (void)co_await cb.call(ok);
        EXPECT_EQ(cb.half_open_successes(), 1u); // fresh start
        co_return;
    });
}

// TC8: reset() test
TEST_F(CircuitBreakerTest, ResetClearsState)
{
    CircuitBreaker cb(CircuitBreakerConfig{
        .failure_threshold = 2, .open_duration = std::chrono::milliseconds{30000}, .half_open_max_calls = 3});

    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto fail = [&]() -> boost::asio::awaitable<Result<void>> {
            co_return std::unexpected(ErrorCode::AdapterError);
        };
        // Trip to OPEN
        for (int i = 0; i < 2; ++i)
            (void)co_await cb.call(fail);
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

// ---------------------------------------------------------------------------
// Concurrency tests — single io_context + multiple co_spawn
// ---------------------------------------------------------------------------

class CircuitBreakerConcurrencyTest : public ::testing::Test
{
  protected:
    boost::asio::io_context io_;
};

// TC10: 단일 io_context에서 4개 코루틴 동시 co_spawn — failure_count 일관성 검증
TEST_F(CircuitBreakerConcurrencyTest, ConcurrentCoroCallsCountConsistent)
{
    CircuitBreaker cb(CircuitBreakerConfig{.failure_threshold = 10, // 높게 설정하여 OPEN 전이 방지 — 순수 카운팅 검증
                                           .open_duration = std::chrono::milliseconds{30000},
                                           .half_open_max_calls = 3});

    constexpr int kCoroCount = 4;
    std::atomic<int> completed{0};

    io_.restart();
    for (int i = 0; i < kCoroCount; ++i)
    {
        boost::asio::co_spawn(
            io_,
            [&]() -> boost::asio::awaitable<void> {
                // co_await post()로 서스펜드 포인트 생성 — 코루틴 인터리빙 환경에서 카운팅 일관성 검증
                auto fail = [&]() -> boost::asio::awaitable<Result<int>> {
                    co_await boost::asio::post(io_, boost::asio::use_awaitable);
                    co_return std::unexpected(ErrorCode::AdapterError);
                };
                auto result = co_await cb.call(fail);
                EXPECT_FALSE(result.has_value());
                EXPECT_EQ(result.error(), ErrorCode::AdapterError);
                ++completed;
                co_return;
            },
            boost::asio::detached);
    }
    io_.run();

    EXPECT_EQ(completed.load(), kCoroCount);
    EXPECT_EQ(cb.failure_count(), static_cast<uint32_t>(kCoroCount));
    EXPECT_EQ(cb.state(), CircuitState::CLOSED); // threshold=10 > 4
}

// TC11: HALF_OPEN 상태에서 half_open_max_calls 스로틀링 검증 — 동시 코루틴 환경
TEST_F(CircuitBreakerConcurrencyTest, HalfOpenThrottlingUnderConcurrency)
{
    constexpr uint32_t kHalfOpenMax = 2;
    CircuitBreaker cb(
        CircuitBreakerConfig{.failure_threshold = 2,
                             .open_duration = std::chrono::milliseconds{1}, // 짧은 open_duration → 빠른 HALF_OPEN 전이
                             .half_open_max_calls = kHalfOpenMax});

    // Phase 1: CLOSED → OPEN 전이
    run_coro(io_, [&]() -> boost::asio::awaitable<void> {
        auto fail = [&]() -> boost::asio::awaitable<Result<int>> {
            co_return std::unexpected(ErrorCode::AdapterError);
        };
        for (uint32_t i = 0; i < 2; ++i)
            (void)co_await cb.call(fail);
        EXPECT_EQ(cb.state(), CircuitState::OPEN);
        co_return;
    });

    // Phase 2: open_duration 경과 대기 후, 4개 코루틴 동시 co_spawn
    //   fn() 내부에 co_await post()를 넣어 서스펜드 포인트를 생성하면,
    //   should_allow()만 순차 통과한 뒤 fn() 실행은 인터리빙된다.
    //   - 코루틴 1: should_allow() → OPEN→HALF_OPEN (calls=1), 허용 → fn에서 서스펜드
    //   - 코루틴 2: should_allow() → HALF_OPEN, calls=1<2 → ++calls=2, 허용 → fn에서 서스펜드
    //   - 코루틴 3: should_allow() → calls=2>=2 → 차단 (CircuitOpen)
    //   - 코루틴 4: 동일하게 차단
    //   - 코루틴 1,2 resume → on_failure()
    constexpr int kCoroCount = 4;
    std::atomic<int> allowed{0};
    std::atomic<int> rejected{0};

    io_.restart();
    // 타이머로 open_duration 경과 보장
    boost::asio::co_spawn(
        io_,
        [&]() -> boost::asio::awaitable<void> {
            boost::asio::steady_timer timer(io_, std::chrono::milliseconds{5});
            co_await timer.async_wait(boost::asio::use_awaitable);

            // 타이머 완료 후 4개 코루틴 co_spawn
            for (int i = 0; i < kCoroCount; ++i)
            {
                boost::asio::co_spawn(
                    io_,
                    [&]() -> boost::asio::awaitable<void> {
                        // fn 내부에 co_await post()로 서스펜드 포인트 생성 — 코루틴 인터리빙 유도
                        auto fail_with_yield = [&]() -> boost::asio::awaitable<Result<int>> {
                            co_await boost::asio::post(io_, boost::asio::use_awaitable);
                            co_return std::unexpected(ErrorCode::AdapterError);
                        };
                        auto result = co_await cb.call(fail_with_yield);
                        if (result.has_value() || result.error() == ErrorCode::AdapterError)
                        {
                            // call()이 fn을 실행했음 (허용됨)
                            ++allowed;
                        }
                        else
                        {
                            // CircuitOpen으로 차단됨
                            EXPECT_EQ(result.error(), ErrorCode::CircuitOpen);
                            ++rejected;
                        }
                        co_return;
                    },
                    boost::asio::detached);
            }
            co_return;
        },
        boost::asio::detached);
    io_.run();

    EXPECT_EQ(allowed.load(), static_cast<int>(kHalfOpenMax));
    EXPECT_EQ(rejected.load(), kCoroCount - static_cast<int>(kHalfOpenMax));
}
