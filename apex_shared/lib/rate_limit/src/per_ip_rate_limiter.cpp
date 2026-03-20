// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/rate_limit/per_ip_rate_limiter.hpp>

#include <algorithm>
#include <cassert>

namespace apex::shared::rate_limit
{

PerIpRateLimiter::PerIpRateLimiter(PerIpRateLimiterConfig config, ScheduleCallback schedule_fn,
                                   CancelCallback cancel_fn, RescheduleCallback reschedule_fn)
    : config_(config)
    , per_core_limit_(std::max(1u, config.total_limit / std::max(1u, config.num_cores)))
    , ttl_(std::chrono::duration_cast<std::chrono::milliseconds>(config.window_size * config.ttl_multiplier))
    , schedule_fn_(std::move(schedule_fn))
    , cancel_fn_(std::move(cancel_fn))
    , reschedule_fn_(std::move(reschedule_fn))
{
    entries_.reserve(config.max_entries);
    lru_order_.reserve(config.max_entries);
}

PerIpRateLimiter::~PerIpRateLimiter()
{
    // Cancel all timers
    for (auto& [ip, entry] : entries_)
    {
        if (entry.timer_handle != 0)
        {
            cancel_fn_(entry.timer_handle);
        }
    }
}

bool PerIpRateLimiter::allow(std::string_view ip, SlidingWindowCounter::TimePoint now)
{
    auto it = entries_.find(std::string{ip});

    if (it == entries_.end())
    {
        // New IP -- check capacity
        if (entries_.size() >= config_.max_entries)
        {
            evict_lru();
        }

        // Schedule TTL expiration callback
        std::string ip_copy(ip);
        auto handle = schedule_fn_(ttl_, [this, ip_copy]() { remove_entry(ip_copy); });

        // Create new entry
        auto [new_it, inserted] =
            entries_.emplace(std::string(ip), Entry{
                                                  .counter = SlidingWindowCounter(per_core_limit_, config_.window_size),
                                                  .timer_handle = handle,
                                                  .lru_index = static_cast<uint32_t>(lru_order_.size()),
                                              });
        lru_order_.emplace_back(std::string(ip));

        return new_it->second.counter.allow(now);
    }

    auto& entry = it->second;

    // Reschedule TTL timer
    reschedule_fn_(entry.timer_handle, ttl_);

    // Update LRU
    touch_lru(entry, entry.lru_index);

    return entry.counter.allow(now);
}

uint32_t PerIpRateLimiter::entry_count() const noexcept
{
    return static_cast<uint32_t>(entries_.size());
}

void PerIpRateLimiter::update_config(PerIpRateLimiterConfig config)
{
    // Cancel all existing timers
    for (auto& [ip, entry] : entries_)
    {
        if (entry.timer_handle != 0)
        {
            cancel_fn_(entry.timer_handle);
        }
    }
    entries_.clear();
    lru_order_.clear();

    config_ = config;
    per_core_limit_ = std::max(1u, config.total_limit / std::max(1u, config.num_cores));
    entries_.reserve(config.max_entries);
    lru_order_.reserve(config.max_entries);

    ttl_ = std::chrono::duration_cast<std::chrono::milliseconds>(config.window_size * config.ttl_multiplier);
}

void PerIpRateLimiter::evict_lru() noexcept
{
    if (lru_order_.empty())
        return;

    // Front of lru_order_ is the oldest entry
    auto& oldest_ip = lru_order_.front();
    auto it = entries_.find(oldest_ip);
    if (it != entries_.end())
    {
        if (it->second.timer_handle != 0)
        {
            cancel_fn_(it->second.timer_handle);
        }
        entries_.erase(it);
    }

    // Remove from LRU: swap front with back, pop back
    if (lru_order_.size() > 1)
    {
        auto& back_ip = lru_order_.back();
        auto back_it = entries_.find(back_ip);
        if (back_it != entries_.end())
        {
            back_it->second.lru_index = 0;
        }
        std::swap(lru_order_.front(), lru_order_.back());
    }
    lru_order_.pop_back();
}

void PerIpRateLimiter::remove_entry(std::string_view ip) noexcept
{
    auto it = entries_.find(std::string{ip});
    if (it == entries_.end())
        return;

    auto lru_idx = it->second.lru_index;

    // Timer already fired — no need to cancel, just clear handle
    entries_.erase(it);

    // Remove from LRU order
    if (lru_idx < lru_order_.size())
    {
        if (lru_idx != lru_order_.size() - 1)
        {
            auto& back_ip = lru_order_.back();
            auto back_it = entries_.find(back_ip);
            if (back_it != entries_.end())
            {
                back_it->second.lru_index = lru_idx;
            }
            std::swap(lru_order_[lru_idx], lru_order_.back());
        }
        lru_order_.pop_back();
    }
}

void PerIpRateLimiter::touch_lru(Entry& entry, uint32_t map_index) noexcept
{
    // Move to back (most recently used)
    if (map_index >= lru_order_.size())
        return;
    auto last_idx = static_cast<uint32_t>(lru_order_.size() - 1);
    if (map_index == last_idx)
        return; // Already at back

    // Swap with back
    auto& back_ip = lru_order_[last_idx];
    auto back_it = entries_.find(back_ip);
    if (back_it != entries_.end())
    {
        back_it->second.lru_index = map_index;
    }
    entry.lru_index = last_idx;
    std::swap(lru_order_[map_index], lru_order_[last_idx]);
}

} // namespace apex::shared::rate_limit
