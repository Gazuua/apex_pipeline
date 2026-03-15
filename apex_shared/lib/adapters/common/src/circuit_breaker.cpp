#include <apex/shared/adapters/circuit_breaker.hpp>

namespace apex::shared::adapters {

CircuitBreaker::CircuitBreaker(CircuitBreakerConfig config)
    : config_(config) {}

CircuitState CircuitBreaker::state() const noexcept { return state_; }
uint32_t CircuitBreaker::failure_count() const noexcept { return failure_count_; }

void CircuitBreaker::reset() noexcept {
    state_ = CircuitState::CLOSED;
    failure_count_ = 0;
    half_open_calls_ = 0;
}

bool CircuitBreaker::should_allow() noexcept {
    switch (state_) {
        case CircuitState::CLOSED:
            return true;
        case CircuitState::OPEN: {
            auto elapsed = std::chrono::steady_clock::now() - open_since_;
            if (elapsed >= config_.open_duration) {
                state_ = CircuitState::HALF_OPEN;
                half_open_calls_ = 0;
                return true;
            }
            return false;
        }
        case CircuitState::HALF_OPEN:
            return half_open_calls_ < config_.half_open_max_calls;
    }
    return false;
}

void CircuitBreaker::on_success() noexcept {
    switch (state_) {
        case CircuitState::HALF_OPEN:
            ++half_open_calls_;
            if (half_open_calls_ >= config_.half_open_max_calls) {
                state_ = CircuitState::CLOSED;
                failure_count_ = 0;
            }
            break;
        case CircuitState::CLOSED:
            failure_count_ = 0;
            break;
        default:
            break;
    }
}

void CircuitBreaker::on_failure() noexcept {
    switch (state_) {
        case CircuitState::CLOSED:
            ++failure_count_;
            if (failure_count_ >= config_.failure_threshold) {
                state_ = CircuitState::OPEN;
                open_since_ = std::chrono::steady_clock::now();
            }
            break;
        case CircuitState::HALF_OPEN:
            state_ = CircuitState::OPEN;
            open_since_ = std::chrono::steady_clock::now();
            break;
        default:
            break;
    }
}

} // namespace apex::shared::adapters
