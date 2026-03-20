// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <type_traits>

namespace apex::shared::adapters
{

/// CircuitBreaker::call()이 받을 수 있는 callable의 반환 타입 제약.
/// F()는 반드시 awaitable<Result<T>>를 반환해야 한다.
template <typename F>
concept CircuitCallable = std::invocable<F> && requires { typename std::invoke_result_t<F>::value_type; };

struct CircuitBreakerConfig
{
    uint32_t failure_threshold = 5;
    std::chrono::milliseconds open_duration{30'000};
    uint32_t half_open_max_calls = 3;
};

enum class CircuitState : uint8_t
{
    CLOSED,
    OPEN,
    HALF_OPEN
};

class CircuitBreaker
{
  public:
    explicit CircuitBreaker(CircuitBreakerConfig config);

    template <CircuitCallable F> [[nodiscard]] std::invoke_result_t<F> call(F&& fn);

    [[nodiscard]] CircuitState state() const noexcept;
    [[nodiscard]] uint32_t failure_count() const noexcept;
    void reset() noexcept;

  private:
    bool should_allow() noexcept;
    void on_success() noexcept;
    void on_failure() noexcept;

    CircuitBreakerConfig config_;
    CircuitState state_ = CircuitState::CLOSED;
    uint32_t failure_count_ = 0;
    uint32_t half_open_calls_ = 0;
    std::chrono::steady_clock::time_point open_since_;
};

// Template implementation
template <CircuitCallable F> [[nodiscard]] std::invoke_result_t<F> CircuitBreaker::call(F&& fn)
{
    if (!should_allow())
    {
        co_return std::unexpected(apex::core::ErrorCode::CircuitOpen);
    }

    auto result = co_await std::forward<F>(fn)();
    if (result.has_value())
    {
        on_success();
    }
    else
    {
        on_failure();
    }
    co_return result;
}

} // namespace apex::shared::adapters
