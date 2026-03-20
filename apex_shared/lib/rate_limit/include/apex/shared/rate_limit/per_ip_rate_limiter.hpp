// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/shared/rate_limit/sliding_window_counter.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// boost::unordered_flat_map for O(1) IP lookup
#include <boost/unordered/unordered_flat_map.hpp>

namespace apex::shared::rate_limit
{

struct PerIpRateLimiterConfig
{
    uint32_t total_limit = 1000;          ///< 전체 한도 (per-core 한도 = total / num_cores)
    std::chrono::seconds window_size{60}; ///< 윈도우 크기
    uint32_t num_cores = 1;               ///< 코어 수 (per-core 한도 계산용)
    uint32_t max_entries = 65536;         ///< 최대 IP 엔트리 수
    uint32_t ttl_multiplier = 2;          ///< TTL = window_size * ttl_multiplier
};

/// Callback type for scheduling delayed tasks.
/// The caller provides a duration and a task; the implementation arranges
/// for the task to execute after the duration elapses.
/// Returns a handle (uint64_t) that can be passed to CancelCallback.
using ScheduleCallback = std::function<uint64_t(std::chrono::milliseconds delay, std::function<void()> task)>;

/// Callback type for cancelling a previously scheduled task.
using CancelCallback = std::function<void(uint64_t handle)>;

/// Callback type for rescheduling an existing task with a new delay.
using RescheduleCallback = std::function<void(uint64_t handle, std::chrono::milliseconds delay)>;

/// Per-IP rate limiter with per-core isolation (lock-free).
///
/// Each core owns one instance. The per-core limit = total_limit / num_cores.
/// Uses boost::unordered_flat_map<string, Entry> for O(1) IP lookup.
/// TTL expiration via injected schedule/cancel callbacks (dependency inversion).
/// LRU eviction when max_entries exceeded.
///
/// Prerequisite: SO_REUSEPORT ensures connections are distributed across cores.
/// If a core receives disproportionate traffic, its per-core limit triggers
/// earlier -- this is intentionally conservative behavior.
///
/// Thread safety: NOT thread-safe. Designed for per-core use (shared-nothing).
class PerIpRateLimiter
{
  public:
    /// @param config Rate limiter configuration.
    /// @param schedule_fn Callback to schedule a delayed task. Returns a handle.
    /// @param cancel_fn Callback to cancel a scheduled task by handle.
    /// @param reschedule_fn Callback to reschedule an existing task with new delay.
    PerIpRateLimiter(PerIpRateLimiterConfig config, ScheduleCallback schedule_fn, CancelCallback cancel_fn,
                     RescheduleCallback reschedule_fn);

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
    [[nodiscard]] bool allow(std::string_view ip, SlidingWindowCounter::TimePoint now);

    /// Number of tracked IP entries.
    [[nodiscard]] uint32_t entry_count() const noexcept;

    /// Per-core limit (total_limit / num_cores).
    [[nodiscard]] uint32_t per_core_limit() const noexcept
    {
        return per_core_limit_;
    }

    /// Update configuration at runtime (TOML hot-reload).
    /// Resets all existing counters.
    void update_config(PerIpRateLimiterConfig config);

  private:
    struct Entry
    {
        SlidingWindowCounter counter;
        uint64_t timer_handle{0};
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
    std::chrono::milliseconds ttl_;

    ScheduleCallback schedule_fn_;
    CancelCallback cancel_fn_;
    RescheduleCallback reschedule_fn_;

    boost::unordered_flat_map<std::string, Entry> entries_;

    /// LRU order: vector of IP strings, front = oldest, back = newest.
    /// Uses swap-to-back for O(1) touch.
    std::vector<std::string> lru_order_;
};

} // namespace apex::shared::rate_limit
