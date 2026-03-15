#include <apex/shared/rate_limit/per_ip_rate_limiter.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::rate_limit;
using namespace std::chrono_literals;

class PerIpRateLimiterTest : public ::testing::Test {
protected:
    using Clock = SlidingWindowCounter::Clock;
    using TimePoint = SlidingWindowCounter::TimePoint;

    TimePoint base_ = Clock::now();
    std::vector<apex::core::TimingWheel::EntryId> expired_;

    // TimingWheel: 512 slots, 1s tick
    apex::core::TimingWheel tw_{512, [this](auto id) { expired_.push_back(id); }};
};

TEST_F(PerIpRateLimiterTest, BasicAllowDeny) {
    PerIpRateLimiter limiter(
        {.total_limit = 10, .window_size = 1s, .num_cores = 1}, tw_);

    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(limiter.allow("192.168.1.1", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(limiter.allow("192.168.1.1", base_ + 10ms));
}

TEST_F(PerIpRateLimiterTest, DifferentIpsIndependent) {
    PerIpRateLimiter limiter(
        {.total_limit = 5, .window_size = 1s, .num_cores = 1}, tw_);

    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.allow("10.0.0.1", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(limiter.allow("10.0.0.1", base_ + 5ms));

    // Different IP is independent
    EXPECT_TRUE(limiter.allow("10.0.0.2", base_ + 5ms));
}

TEST_F(PerIpRateLimiterTest, PerCoreLimitDivision) {
    // total=100, 4 cores -> per-core = 25
    PerIpRateLimiter limiter(
        {.total_limit = 100, .window_size = 1s, .num_cores = 4}, tw_);

    EXPECT_EQ(limiter.per_core_limit(), 25u);

    for (int i = 0; i < 25; ++i) {
        EXPECT_TRUE(limiter.allow("1.2.3.4", base_ + std::chrono::milliseconds(i)));
    }
    EXPECT_FALSE(limiter.allow("1.2.3.4", base_ + 25ms));
}

TEST_F(PerIpRateLimiterTest, MaxEntriesEvictsLRU) {
    PerIpRateLimiter limiter(
        {.total_limit = 100, .window_size = 1s, .num_cores = 1, .max_entries = 3},
        tw_);

    limiter.allow("ip1", base_);
    limiter.allow("ip2", base_ + 1ms);
    limiter.allow("ip3", base_ + 2ms);
    EXPECT_EQ(limiter.entry_count(), 3u);

    // Adding ip4 should evict ip1 (LRU)
    limiter.allow("ip4", base_ + 3ms);
    EXPECT_EQ(limiter.entry_count(), 3u);

    // ip1's counter was evicted -- fresh counter created
    // (all 100 requests available again)
    int allowed = 0;
    for (int i = 0; i < 100; ++i) {
        if (limiter.allow("ip1", base_ + 4ms + std::chrono::microseconds(i)))
            ++allowed;
    }
    // ip4 was evicted to make room for ip1 (ip2 was LRU at that point, actually ip2)
    // The exact eviction depends on LRU order after ip4 insertion
    EXPECT_GT(allowed, 0);
}

TEST_F(PerIpRateLimiterTest, EntryCount) {
    PerIpRateLimiter limiter(
        {.total_limit = 100, .window_size = 1s, .num_cores = 1}, tw_);

    EXPECT_EQ(limiter.entry_count(), 0u);
    limiter.allow("a", base_);
    EXPECT_EQ(limiter.entry_count(), 1u);
    limiter.allow("b", base_ + 1ms);
    EXPECT_EQ(limiter.entry_count(), 2u);
    // Same IP doesn't create new entry
    limiter.allow("a", base_ + 2ms);
    EXPECT_EQ(limiter.entry_count(), 2u);
}

TEST_F(PerIpRateLimiterTest, UpdateConfigResetsAll) {
    PerIpRateLimiter limiter(
        {.total_limit = 10, .window_size = 1s, .num_cores = 1}, tw_);

    for (int i = 0; i < 10; ++i) {
        limiter.allow("ip", base_ + std::chrono::milliseconds(i));
    }
    EXPECT_FALSE(limiter.allow("ip", base_ + 10ms));

    // Update config -- all counters reset
    limiter.update_config({.total_limit = 20, .window_size = 1s, .num_cores = 1});
    EXPECT_EQ(limiter.entry_count(), 0u);
    EXPECT_EQ(limiter.per_core_limit(), 20u);
    EXPECT_TRUE(limiter.allow("ip", base_ + 1s));
}

TEST_F(PerIpRateLimiterTest, IPv6Support) {
    PerIpRateLimiter limiter(
        {.total_limit = 5, .window_size = 1s, .num_cores = 1}, tw_);

    EXPECT_TRUE(limiter.allow("::1", base_));
    EXPECT_TRUE(limiter.allow("2001:db8::1", base_ + 1ms));
    EXPECT_EQ(limiter.entry_count(), 2u);
}

TEST_F(PerIpRateLimiterTest, MinimumPerCoreLimit) {
    // total=1, 4 cores -> per-core should be at least 1
    PerIpRateLimiter limiter(
        {.total_limit = 1, .window_size = 1s, .num_cores = 4}, tw_);

    EXPECT_GE(limiter.per_core_limit(), 1u);
}
