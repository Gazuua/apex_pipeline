#include <apex/shared/rate_limit/per_ip_rate_limiter.hpp>

#include <algorithm>
#include <cassert>

namespace apex::shared::rate_limit {

PerIpRateLimiter::PerIpRateLimiter(PerIpRateLimiterConfig config,
                                   apex::core::TimingWheel& timing_wheel)
    : config_(config),
      per_core_limit_(std::max(1u, config.total_limit / std::max(1u, config.num_cores))),
      timing_wheel_(timing_wheel) {
    entries_.reserve(config.max_entries);
    lru_order_.reserve(config.max_entries);

    // TTL in ticks: window_size * ttl_multiplier
    // Assumes TimingWheel tick interval = 1 second (standard configuration)
    auto ttl_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        config.window_size * config.ttl_multiplier);
    ttl_ticks_ = static_cast<uint32_t>(ttl_seconds.count());
}

PerIpRateLimiter::~PerIpRateLimiter() {
    // Cancel all timers
    for (auto& [ip, entry] : entries_) {
        if (entry.timer_id != 0) {
            timing_wheel_.cancel(entry.timer_id);
        }
    }
}

bool PerIpRateLimiter::allow(std::string_view ip,
                             SlidingWindowCounter::TimePoint now) noexcept {
    auto it = entries_.find(ip);

    if (it == entries_.end()) {
        // New IP -- check capacity
        if (entries_.size() >= config_.max_entries) {
            evict_lru();
        }

        // Create new entry
        auto [new_it, inserted] = entries_.emplace(
            std::string(ip),
            Entry{
                .counter = SlidingWindowCounter(per_core_limit_, config_.window_size),
                .timer_id = timing_wheel_.schedule(ttl_ticks_),
                .lru_index = static_cast<uint32_t>(lru_order_.size()),
            });
        lru_order_.emplace_back(std::string(ip));

        return new_it->second.counter.allow(now);
    }

    auto& entry = it->second;

    // Reschedule TTL timer
    timing_wheel_.reschedule(entry.timer_id, ttl_ticks_);

    // Update LRU
    touch_lru(entry, entry.lru_index);

    return entry.counter.allow(now);
}

uint32_t PerIpRateLimiter::entry_count() const noexcept {
    return static_cast<uint32_t>(entries_.size());
}

void PerIpRateLimiter::update_config(PerIpRateLimiterConfig config) noexcept {
    // Cancel all existing timers
    for (auto& [ip, entry] : entries_) {
        if (entry.timer_id != 0) {
            timing_wheel_.cancel(entry.timer_id);
        }
    }
    entries_.clear();
    lru_order_.clear();

    config_ = config;
    per_core_limit_ = std::max(1u, config.total_limit / std::max(1u, config.num_cores));
    entries_.reserve(config.max_entries);
    lru_order_.reserve(config.max_entries);

    auto ttl_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        config.window_size * config.ttl_multiplier);
    ttl_ticks_ = static_cast<uint32_t>(ttl_seconds.count());
}

void PerIpRateLimiter::evict_lru() noexcept {
    if (lru_order_.empty()) return;

    // Front of lru_order_ is the oldest entry
    auto& oldest_ip = lru_order_.front();
    auto it = entries_.find(oldest_ip);
    if (it != entries_.end()) {
        if (it->second.timer_id != 0) {
            timing_wheel_.cancel(it->second.timer_id);
        }
        entries_.erase(it);
    }

    // Remove from LRU: swap front with back, pop back
    if (lru_order_.size() > 1) {
        auto& back_ip = lru_order_.back();
        auto back_it = entries_.find(back_ip);
        if (back_it != entries_.end()) {
            back_it->second.lru_index = 0;
        }
        std::swap(lru_order_.front(), lru_order_.back());
    }
    lru_order_.pop_back();
}

void PerIpRateLimiter::remove_entry(std::string_view ip) noexcept {
    auto it = entries_.find(ip);
    if (it == entries_.end()) return;

    auto lru_idx = it->second.lru_index;

    if (it->second.timer_id != 0) {
        timing_wheel_.cancel(it->second.timer_id);
    }
    entries_.erase(it);

    // Remove from LRU order
    if (lru_idx < lru_order_.size()) {
        if (lru_idx != lru_order_.size() - 1) {
            auto& back_ip = lru_order_.back();
            auto back_it = entries_.find(back_ip);
            if (back_it != entries_.end()) {
                back_it->second.lru_index = lru_idx;
            }
            std::swap(lru_order_[lru_idx], lru_order_.back());
        }
        lru_order_.pop_back();
    }
}

void PerIpRateLimiter::touch_lru(Entry& entry, uint32_t map_index) noexcept {
    // Move to back (most recently used)
    if (map_index >= lru_order_.size()) return;
    auto last_idx = static_cast<uint32_t>(lru_order_.size() - 1);
    if (map_index == last_idx) return;  // Already at back

    // Swap with back
    auto& back_ip = lru_order_[last_idx];
    auto back_it = entries_.find(back_ip);
    if (back_it != entries_.end()) {
        back_it->second.lru_index = map_index;
    }
    entry.lru_index = last_idx;
    std::swap(lru_order_[map_index], lru_order_[last_idx]);
}

} // namespace apex::shared::rate_limit
