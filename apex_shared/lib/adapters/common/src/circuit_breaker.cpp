// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/circuit_breaker.hpp>

namespace apex::shared::adapters
{

CircuitBreaker::CircuitBreaker(CircuitBreakerConfig config)
    : config_(config)
{}

CircuitState CircuitBreaker::state() const noexcept
{
    return state_;
}
uint32_t CircuitBreaker::failure_count() const noexcept
{
    return failure_count_;
}
uint32_t CircuitBreaker::half_open_successes() const noexcept
{
    return half_open_successes_;
}

void CircuitBreaker::reset() noexcept
{
    logger_.info("reset");
    state_ = CircuitState::CLOSED;
    failure_count_ = 0;
    half_open_calls_ = 0;
    half_open_successes_ = 0;
}

bool CircuitBreaker::should_allow() noexcept
{
    switch (state_)
    {
        case CircuitState::CLOSED:
            return true;
        case CircuitState::OPEN:
        {
            auto elapsed = std::chrono::steady_clock::now() - open_since_;
            if (elapsed >= config_.open_duration)
            {
                logger_.info("state OPEN->HALF_OPEN after {}ms",
                             std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
                state_ = CircuitState::HALF_OPEN;
                half_open_calls_ = 1;     // 이 호출 자체를 첫 번째로 카운팅
                half_open_successes_ = 0; // 성공 카운터 초기화
                return true;
            }
            return false;
        }
        case CircuitState::HALF_OPEN:
            if (half_open_calls_ >= config_.half_open_max_calls)
                return false;
            ++half_open_calls_; // 진입 시점에 즉시 카운팅 (코루틴 인터리빙 방어)
            return true;
    }
    return false;
}

void CircuitBreaker::on_success() noexcept
{
    switch (state_)
    {
        case CircuitState::HALF_OPEN:
            // 성공 카운터로 CLOSED 전이 판단 — half_open_calls_와 분리하여
            // "허용된 호출 수"와 "성공 횟수"의 의미를 명확히 구분한다.
            ++half_open_successes_;
            logger_.debug("on_success HALF_OPEN successes={}/{}", half_open_successes_, config_.half_open_max_calls);
            if (half_open_successes_ >= config_.half_open_max_calls)
            {
                logger_.info("state HALF_OPEN->CLOSED");
                state_ = CircuitState::CLOSED;
                failure_count_ = 0;
                half_open_successes_ = 0;
            }
            break;
        case CircuitState::CLOSED:
            failure_count_ = 0;
            break;
        default:
            break;
    }
}

void CircuitBreaker::on_failure() noexcept
{
    switch (state_)
    {
        case CircuitState::CLOSED:
            ++failure_count_;
            if (failure_count_ >= config_.failure_threshold)
            {
                logger_.warn("state CLOSED->OPEN failures={}/{}", failure_count_, config_.failure_threshold);
                state_ = CircuitState::OPEN;
                open_since_ = std::chrono::steady_clock::now();
            }
            break;
        case CircuitState::HALF_OPEN:
            logger_.warn("state HALF_OPEN->OPEN (probe failed)");
            state_ = CircuitState::OPEN;
            open_since_ = std::chrono::steady_clock::now();
            break;
        default:
            break;
    }
}

} // namespace apex::shared::adapters
