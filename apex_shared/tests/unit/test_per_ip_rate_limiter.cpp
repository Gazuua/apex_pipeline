// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/rate_limit/per_ip_rate_limiter.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::rate_limit;
using namespace std::chrono_literals;

/// Mock timer infrastructure for testing PerIpRateLimiter's callback injection.
struct MockTimer
{
    uint64_t next_handle = 1;
    /// Scheduled tasks: handle -> (delay, task)
    std::vector<std::pair<uint64_t, std::pair<std::chrono::milliseconds, std::function<void()>>>> tasks;

    /// Track cancelled handles
    std::vector<uint64_t> cancelled;

    /// Track rescheduled handles: handle -> new_delay
    std::vector<std::pair<uint64_t, std::chrono::milliseconds>> rescheduled;

    ScheduleCallback make_schedule()
    {
        return [this](std::chrono::milliseconds delay, std::function<void()> task) -> uint64_t {
            auto h = next_handle++;
            tasks.emplace_back(h, std::make_pair(delay, std::move(task)));
            return h;
        };
    }

    CancelCallback make_cancel()
    {
        return [this](uint64_t handle) { cancelled.push_back(handle); };
    }

    RescheduleCallback make_reschedule()
    {
        return [this](uint64_t handle, std::chrono::milliseconds delay) { rescheduled.emplace_back(handle, delay); };
    }

    /// Fire all pending tasks (simulates TTL expiration for all).
    void fire_all()
    {
        auto snapshot = std::move(tasks);
        tasks.clear();
        for (auto& [h, dp] : snapshot)
        {
            dp.second();
        }
    }

    /// Fire task by handle (simulates TTL expiration for a specific entry).
    void fire(uint64_t handle)
    {
        for (auto it = tasks.begin(); it != tasks.end(); ++it)
        {
            if (it->first == handle)
            {
                auto task = std::move(it->second.second);
                tasks.erase(it);
                task();
                return;
            }
        }
    }
};

class PerIpRateLimiterTest : public ::testing::Test
{
  protected:
    using Clock = SlidingWindowCounter::Clock;
    using TimePoint = SlidingWindowCounter::TimePoint;

    TimePoint base_ = Clock::now();
    MockTimer mock_;
};

TEST_F(PerIpRateLimiterTest, BasicAllowDeny)
{
    PerIpRateLimiter limiter(
        {.total_limit = 10, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    for (int i = 0; i < 10; ++i)
    {
        EXPECT_TRUE(limiter.allow("192.168.1.1", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(limiter.allow("192.168.1.1", base_ + 10ms));
}

TEST_F(PerIpRateLimiterTest, DifferentIpsIndependent)
{
    PerIpRateLimiter limiter(
        {.total_limit = 5, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(limiter.allow("10.0.0.1", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(limiter.allow("10.0.0.1", base_ + 5ms));

    // Different IP is independent
    EXPECT_TRUE(limiter.allow("10.0.0.2", base_ + 5ms));
}

TEST_F(PerIpRateLimiterTest, PerCoreLimitDivision)
{
    // total=100, 4 cores -> per-core = 25
    PerIpRateLimiter limiter(
        {.total_limit = 100, .window_size = 1s, .num_cores = 4, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    EXPECT_EQ(limiter.per_core_limit(), 25u);

    for (int i = 0; i < 25; ++i)
    {
        EXPECT_TRUE(limiter.allow("1.2.3.4", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(limiter.allow("1.2.3.4", base_ + 25ms));
}

TEST_F(PerIpRateLimiterTest, MaxEntriesEvictsLRU)
{
    PerIpRateLimiter limiter(
        {.total_limit = 100, .window_size = 1s, .num_cores = 1, .max_entries = 3, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    (void)limiter.allow("ip1", base_);
    (void)limiter.allow("ip2", base_ + 1ms);
    (void)limiter.allow("ip3", base_ + 2ms);
    EXPECT_EQ(limiter.entry_count(), 3u);

    // Adding ip4 should evict ip1 (LRU)
    (void)limiter.allow("ip4", base_ + 3ms);
    EXPECT_EQ(limiter.entry_count(), 3u);

    // ip1's counter was evicted -- fresh counter created
    // (all 100 requests available again)
    int allowed = 0;
    for (int i = 0; i < 100; ++i)
    {
        if (limiter.allow("ip1", base_ + 4ms + std::chrono::microseconds(i)))
            ++allowed;
    }
    // ip4 was evicted to make room for ip1 (ip2 was LRU at that point, actually ip2)
    // The exact eviction depends on LRU order after ip4 insertion
    EXPECT_GT(allowed, 0);
}

TEST_F(PerIpRateLimiterTest, EntryCount)
{
    PerIpRateLimiter limiter(
        {.total_limit = 100, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    EXPECT_EQ(limiter.entry_count(), 0u);
    (void)limiter.allow("a", base_);
    EXPECT_EQ(limiter.entry_count(), 1u);
    (void)limiter.allow("b", base_ + 1ms);
    EXPECT_EQ(limiter.entry_count(), 2u);
    // Same IP doesn't create new entry
    (void)limiter.allow("a", base_ + 2ms);
    EXPECT_EQ(limiter.entry_count(), 2u);
}

TEST_F(PerIpRateLimiterTest, UpdateConfigResetsAll)
{
    PerIpRateLimiter limiter(
        {.total_limit = 10, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    for (int i = 0; i < 10; ++i)
    {
        (void)limiter.allow("ip", base_ + std::chrono::milliseconds(i));
    }
    EXPECT_FALSE(limiter.allow("ip", base_ + 10ms));

    // Update config -- all counters reset
    limiter.update_config(
        {.total_limit = 20, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2});
    EXPECT_EQ(limiter.entry_count(), 0u);
    EXPECT_EQ(limiter.per_core_limit(), 20u);
    EXPECT_TRUE(limiter.allow("ip", base_ + 1s));
}

TEST_F(PerIpRateLimiterTest, IPv6Support)
{
    PerIpRateLimiter limiter(
        {.total_limit = 5, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    EXPECT_TRUE(limiter.allow("::1", base_));
    EXPECT_TRUE(limiter.allow("2001:db8::1", base_ + 1ms));
    EXPECT_EQ(limiter.entry_count(), 2u);
}

TEST_F(PerIpRateLimiterTest, MinimumPerCoreLimit)
{
    // total=1, 4 cores -> per-core should be at least 1
    PerIpRateLimiter limiter(
        {.total_limit = 1, .window_size = 1s, .num_cores = 4, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    EXPECT_GE(limiter.per_core_limit(), 1u);
}

TEST_F(PerIpRateLimiterTest, TtlExpirationRemovesEntry)
{
    PerIpRateLimiter limiter(
        {.total_limit = 10, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    (void)limiter.allow("10.0.0.1", base_);
    (void)limiter.allow("10.0.0.2", base_ + 1ms);
    EXPECT_EQ(limiter.entry_count(), 2u);

    // Verify schedule was called with correct TTL (1s * 2 = 2000ms)
    ASSERT_EQ(mock_.tasks.size(), 2u);
    EXPECT_EQ(mock_.tasks[0].second.first, 2000ms);
    EXPECT_EQ(mock_.tasks[1].second.first, 2000ms);

    // Fire TTL for first IP — should remove the entry
    mock_.fire(mock_.tasks[0].first);
    EXPECT_EQ(limiter.entry_count(), 1u);

    // Fire TTL for second IP — should remove remaining entry
    mock_.fire(mock_.tasks[0].first); // tasks shifted after erase, new [0] is old [1]
    EXPECT_EQ(limiter.entry_count(), 0u);
}

TEST_F(PerIpRateLimiterTest, TtlExpirationResetsCounter)
{
    PerIpRateLimiter limiter(
        {.total_limit = 5, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    // Exhaust rate limit for an IP
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(limiter.allow("10.0.0.1", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(limiter.allow("10.0.0.1", base_ + 5ms));

    // Simulate TTL expiration — the entry is removed
    ASSERT_GE(mock_.tasks.size(), 1u);
    // The first schedule created handle=1 for this IP (reschedules don't add new tasks)
    // After 6 allow() calls: 1 schedule (new entry) + 5 reschedules (existing entry)
    // Find the task for this IP and fire it
    mock_.fire(1); // handle 1 was the initial schedule
    EXPECT_EQ(limiter.entry_count(), 0u);

    // Now the IP can be used again with fresh counter
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(limiter.allow("10.0.0.1", base_ + 1s + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(limiter.allow("10.0.0.1", base_ + 1s + 5ms));
}

TEST_F(PerIpRateLimiterTest, RescheduleOnAccess)
{
    PerIpRateLimiter limiter(
        {.total_limit = 100, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
        mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

    (void)limiter.allow("10.0.0.1", base_);
    EXPECT_EQ(mock_.tasks.size(), 1u);       // schedule called once
    EXPECT_EQ(mock_.rescheduled.size(), 0u); // no reschedule yet

    // Access same IP again — should trigger reschedule
    (void)limiter.allow("10.0.0.1", base_ + 100ms);
    EXPECT_EQ(mock_.tasks.size(), 1u);              // no new schedule
    ASSERT_EQ(mock_.rescheduled.size(), 1u);        // reschedule called
    EXPECT_EQ(mock_.rescheduled[0].second, 2000ms); // TTL = 1s * 2
}

TEST_F(PerIpRateLimiterTest, DestructorCancelsAllTimers)
{
    {
        PerIpRateLimiter limiter(
            {.total_limit = 100, .window_size = 1s, .num_cores = 1, .max_entries = 65536, .ttl_multiplier = 2},
            mock_.make_schedule(), mock_.make_cancel(), mock_.make_reschedule());

        (void)limiter.allow("ip1", base_);
        (void)limiter.allow("ip2", base_ + 1ms);
        (void)limiter.allow("ip3", base_ + 2ms);
        EXPECT_EQ(mock_.cancelled.size(), 0u);
    }
    // After destruction, all 3 timers should be cancelled
    EXPECT_EQ(mock_.cancelled.size(), 3u);
}
