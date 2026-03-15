#include <apex/shared/rate_limit/sliding_window_counter.hpp>

namespace apex::shared::rate_limit {

SlidingWindowCounter::SlidingWindowCounter(uint32_t limit, Duration window_size) noexcept
    : limit_(limit), window_size_(window_size) {}

bool SlidingWindowCounter::allow(TimePoint now) noexcept {
    advance_window(now);

    // Compute weighted estimate
    auto elapsed = now - window_start_;
    double ratio = static_cast<double>(elapsed.count()) /
                   static_cast<double>(window_size_.count());
    if (ratio > 1.0) ratio = 1.0;

    double estimate = previous_count_ * (1.0 - ratio) + current_count_;

    if (estimate >= static_cast<double>(limit_)) {
        return false;
    }

    ++current_count_;
    last_access_ = now;
    return true;
}

double SlidingWindowCounter::estimated_count(TimePoint now) const noexcept {
    // Compute without mutating state.
    // We need to figure out what window 'now' falls into.
    auto elapsed_since_start = now - window_start_;

    uint32_t eff_current = current_count_;
    uint32_t eff_previous = previous_count_;
    auto eff_window_start = window_start_;

    if (window_size_.count() > 0 && elapsed_since_start >= window_size_) {
        auto windows_passed =
            elapsed_since_start.count() / window_size_.count();
        if (windows_passed == 1) {
            eff_previous = eff_current;
            eff_current = 0;
            eff_window_start = window_start_ + window_size_;
        } else {
            // 2+ windows passed -- both counters are stale
            return 0.0;
        }
    }

    auto elapsed = now - eff_window_start;
    double ratio = static_cast<double>(elapsed.count()) /
                   static_cast<double>(window_size_.count());
    if (ratio > 1.0) ratio = 1.0;

    return eff_previous * (1.0 - ratio) + eff_current;
}

void SlidingWindowCounter::reset() noexcept {
    current_count_ = 0;
    previous_count_ = 0;
    window_start_ = {};
    last_access_ = {};
}

void SlidingWindowCounter::advance_window(TimePoint now) noexcept {
    // First call -- initialize window
    if (window_start_ == TimePoint{}) {
        window_start_ = now;
        return;
    }

    auto elapsed = now - window_start_;
    if (elapsed < window_size_) {
        return; // Still in current window
    }

    // How many full windows have elapsed?
    auto windows_passed = elapsed.count() / window_size_.count();

    if (windows_passed == 1) {
        // Exactly one window passed: rotate
        previous_count_ = current_count_;
        current_count_ = 0;
        window_start_ += window_size_;
    } else {
        // Two or more windows passed: previous data is completely stale
        previous_count_ = 0;
        current_count_ = 0;
        window_start_ = now;
    }
}

} // namespace apex::shared::rate_limit
