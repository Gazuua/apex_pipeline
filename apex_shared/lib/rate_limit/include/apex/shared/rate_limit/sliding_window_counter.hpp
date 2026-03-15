#pragma once

#include <chrono>
#include <cstdint>

namespace apex::shared::rate_limit {

/// Sliding Window Counter for rate limiting.
/// Tracks request counts across two adjacent windows and computes a
/// weighted estimate to avoid boundary burst issues.
///
/// Thread safety: NOT thread-safe. Designed for per-core use (shared-nothing).
///
/// Usage:
///   SlidingWindowCounter counter(100, std::chrono::seconds{60});
///   if (counter.allow(now)) {
///       // 요청 허용
///   } else {
///       // Rate limit 초과
///   }
class SlidingWindowCounter {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    /// @param limit Maximum allowed requests per window.
    /// @param window_size Duration of one window.
    SlidingWindowCounter(uint32_t limit, Duration window_size) noexcept;

    /// Check if a request is allowed at the given time.
    /// If allowed, increments the counter and returns true.
    /// If denied, does NOT increment and returns false.
    /// @param now Current time point.
    /// @return true if the request is within the rate limit.
    [[nodiscard]] bool allow(TimePoint now) noexcept;

    /// Current estimated count (for monitoring/logging).
    /// Does NOT advance the window.
    [[nodiscard]] double estimated_count(TimePoint now) const noexcept;

    /// Reset all counters and window state.
    void reset() noexcept;

    /// Last time any request was recorded (for LRU eviction).
    [[nodiscard]] TimePoint last_access() const noexcept { return last_access_; }

    // --- Config accessors ---
    [[nodiscard]] uint32_t limit() const noexcept { return limit_; }
    [[nodiscard]] Duration window_size() const noexcept { return window_size_; }

private:
    /// Advance window(s) if needed based on current time.
    void advance_window(TimePoint now) noexcept;

    uint32_t limit_;
    Duration window_size_;

    uint32_t current_count_{0};
    uint32_t previous_count_{0};
    TimePoint window_start_{};
    TimePoint last_access_{};
};

} // namespace apex::shared::rate_limit
