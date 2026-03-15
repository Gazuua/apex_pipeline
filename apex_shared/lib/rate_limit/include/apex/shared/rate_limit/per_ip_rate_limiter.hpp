#pragma once

#include <apex/shared/rate_limit/sliding_window_counter.hpp>
#include <apex/core/timing_wheel.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// boost::unordered_flat_map for O(1) IP lookup
#include <boost/unordered/unordered_flat_map.hpp>

namespace apex::shared::rate_limit {

struct PerIpRateLimiterConfig {
    uint32_t total_limit = 1000;           ///< 전체 한도 (per-core 한도 = total / num_cores)
    std::chrono::seconds window_size{60};  ///< 윈도우 크기
    uint32_t num_cores = 1;                ///< 코어 수 (per-core 한도 계산용)
    uint32_t max_entries = 65536;          ///< 최대 IP 엔트리 수
    uint32_t ttl_multiplier = 2;           ///< TTL = window_size * ttl_multiplier
};

/// Per-IP rate limiter with per-core isolation (lock-free).
///
/// Each core owns one instance. The per-core limit = total_limit / num_cores.
/// Uses boost::unordered_flat_map<string, Entry> for O(1) IP lookup.
/// TTL expiration via TimingWheel, LRU eviction when max_entries exceeded.
///
/// Prerequisite: SO_REUSEPORT ensures connections are distributed across cores.
/// If a core receives disproportionate traffic, its per-core limit triggers
/// earlier -- this is intentionally conservative behavior.
///
/// Thread safety: NOT thread-safe. Designed for per-core use (shared-nothing).
class PerIpRateLimiter {
public:
    /// @param config Rate limiter configuration.
    /// @param timing_wheel Reference to the per-core TimingWheel
    ///        (tick interval must match the limiter's TTL granularity).
    PerIpRateLimiter(PerIpRateLimiterConfig config,
                     apex::core::TimingWheel& timing_wheel);

    ~PerIpRateLimiter();

    // Non-copyable, non-movable
    PerIpRateLimiter(const PerIpRateLimiter&) = delete;
    PerIpRateLimiter& operator=(const PerIpRateLimiter&) = delete;
    PerIpRateLimiter(PerIpRateLimiter&&) = delete;
    PerIpRateLimiter& operator=(PerIpRateLimiter&&) = delete;

    /// Check if a request from the given IP is allowed.
    /// @param ip Client IP address (IPv4 or IPv6 string).
    /// @param now Current time point.
    /// @return true if the request is within the per-core rate limit.
    [[nodiscard]] bool allow(std::string_view ip,
                             SlidingWindowCounter::TimePoint now);

    /// Number of tracked IP entries.
    [[nodiscard]] uint32_t entry_count() const noexcept;

    /// Per-core limit (total_limit / num_cores).
    [[nodiscard]] uint32_t per_core_limit() const noexcept { return per_core_limit_; }

    /// Update configuration at runtime (TOML hot-reload).
    /// Resets all existing counters.
    void update_config(PerIpRateLimiterConfig config);

private:
    struct Entry {
        SlidingWindowCounter counter;
        apex::core::TimingWheel::EntryId timer_id{0};
        /// Index in lru_order_ for O(1) LRU removal.
        uint32_t lru_index{0};
    };

    /// Evict the LRU entry to make room.
    void evict_lru() noexcept;

    /// Remove an entry by IP (called by TTL expiration).
    void remove_entry(std::string_view ip) noexcept;

    /// Touch an entry for LRU tracking (move to back).
    void touch_lru(Entry& entry, uint32_t map_index) noexcept;

    PerIpRateLimiterConfig config_;
    uint32_t per_core_limit_;
    apex::core::TimingWheel& timing_wheel_;
    uint32_t ttl_ticks_;  ///< TTL in TimingWheel ticks

    boost::unordered_flat_map<std::string, Entry> entries_;

    /// LRU order: vector of IP strings, front = oldest, back = newest.
    /// Uses swap-to-back for O(1) touch.
    std::vector<std::string> lru_order_;
};

} // namespace apex::shared::rate_limit
