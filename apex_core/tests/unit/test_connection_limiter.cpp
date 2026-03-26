// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/connection_limiter.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>

using namespace apex::core;

namespace
{

class ConnectionLimiterTest : public ::testing::Test
{
  protected:
    static constexpr uint32_t CORE_ID = 0;
    static constexpr uint32_t NUM_CORES = 4;
    static constexpr uint32_t MAX_PER_IP = 3;

    ConnectionLimiter limiter_{CORE_ID, NUM_CORES, MAX_PER_IP};
};

} // anonymous namespace

TEST_F(ConnectionLimiterTest, BasicIncrementDecrementCycle)
{
    const std::string ip = "10.0.0.1";

    EXPECT_TRUE(limiter_.try_increment(ip));
    EXPECT_EQ(limiter_.count(ip), 1u);

    limiter_.decrement(ip);
    EXPECT_EQ(limiter_.count(ip), 0u);
}

TEST_F(ConnectionLimiterTest, MaxPerIpRejectsWhenLimitReached)
{
    const std::string ip = "10.0.0.2";

    // Fill up to the limit
    for (uint32_t i = 0; i < MAX_PER_IP; ++i)
    {
        EXPECT_TRUE(limiter_.try_increment(ip)) << "increment #" << i << " should succeed";
    }
    EXPECT_EQ(limiter_.count(ip), MAX_PER_IP);

    // Next attempt should be rejected
    EXPECT_FALSE(limiter_.try_increment(ip));
    // Count should remain at the limit
    EXPECT_EQ(limiter_.count(ip), MAX_PER_IP);
}

TEST_F(ConnectionLimiterTest, EmptyIpRejected)
{
    EXPECT_FALSE(limiter_.try_increment(""));
    EXPECT_EQ(limiter_.tracked_ips(), 0u);
}

TEST_F(ConnectionLimiterTest, DecrementUnderflowDefense)
{
    const std::string ip = "10.0.0.3";

    // Decrement for an IP that was never incremented — should not crash
    limiter_.decrement(ip);
    EXPECT_EQ(limiter_.count(ip), 0u);

    // Increment once, decrement twice — second decrement is underflow
    EXPECT_TRUE(limiter_.try_increment(ip));
    limiter_.decrement(ip);
    EXPECT_EQ(limiter_.count(ip), 0u);

    limiter_.decrement(ip);
    EXPECT_EQ(limiter_.count(ip), 0u);
}

TEST_F(ConnectionLimiterTest, OwnerCoreHashConsistency)
{
    // Same IP always maps to the same core
    const std::string ip = "192.168.1.100";
    auto core = ConnectionLimiter::owner_core(ip, NUM_CORES);

    for (int i = 0; i < 100; ++i)
    {
        EXPECT_EQ(ConnectionLimiter::owner_core(ip, NUM_CORES), core);
    }

    // Result is within valid range
    EXPECT_LT(core, NUM_CORES);
}

TEST_F(ConnectionLimiterTest, OwnerCoreDistributesDifferentIPs)
{
    // Different IPs should (probabilistically) distribute across cores.
    // With enough IPs and 4 cores, we should see at least 2 distinct cores.
    std::set<uint32_t> seen_cores;
    for (int i = 0; i < 100; ++i)
    {
        std::string ip = "10.0.0." + std::to_string(i);
        seen_cores.insert(ConnectionLimiter::owner_core(ip, NUM_CORES));
    }
    EXPECT_GE(seen_cores.size(), 2u);
}

TEST_F(ConnectionLimiterTest, TrackedIpsCount)
{
    EXPECT_EQ(limiter_.tracked_ips(), 0u);

    EXPECT_TRUE(limiter_.try_increment("1.1.1.1"));
    EXPECT_TRUE(limiter_.try_increment("2.2.2.2"));
    EXPECT_TRUE(limiter_.try_increment("3.3.3.3"));
    EXPECT_EQ(limiter_.tracked_ips(), 3u);

    // Incrementing an existing IP doesn't add a new entry
    EXPECT_TRUE(limiter_.try_increment("1.1.1.1"));
    EXPECT_EQ(limiter_.tracked_ips(), 3u);

    // Decrementing to 0 removes the entry
    limiter_.decrement("1.1.1.1");
    limiter_.decrement("1.1.1.1");
    EXPECT_EQ(limiter_.tracked_ips(), 2u);
}

TEST_F(ConnectionLimiterTest, LimitReachedThenDecrementAllowsNewIncrement)
{
    const std::string ip = "10.0.0.4";

    // Fill to limit
    for (uint32_t i = 0; i < MAX_PER_IP; ++i)
    {
        EXPECT_TRUE(limiter_.try_increment(ip));
    }
    EXPECT_FALSE(limiter_.try_increment(ip));

    // Free one slot
    limiter_.decrement(ip);
    EXPECT_EQ(limiter_.count(ip), MAX_PER_IP - 1);

    // Now increment should succeed again
    EXPECT_TRUE(limiter_.try_increment(ip));
    EXPECT_EQ(limiter_.count(ip), MAX_PER_IP);
}

TEST_F(ConnectionLimiterTest, ZeroLimitAsserts)
{
    // Constructor asserts max_per_ip > 0
#ifdef NDEBUG
    GTEST_SKIP() << "assert disabled in Release build";
#else
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH((ConnectionLimiter{0, 4, 0}), "");
#endif
}
