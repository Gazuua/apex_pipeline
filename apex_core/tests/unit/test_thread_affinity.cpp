// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/cpu_topology.hpp>
#include <apex/core/thread_affinity.hpp>

#include <gtest/gtest.h>

#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <sched.h>
#endif

using namespace apex::core;

TEST(ThreadAffinityTest, PinToFirstLogicalCore)
{
    auto topo = discover_topology();
    ASSERT_FALSE(topo.physical_cores.empty());

    auto target = topo.physical_cores.front().primary_logical_id();
    EXPECT_TRUE(apply_thread_affinity(target));

    // Verify we're running on the expected core
#ifdef _WIN32
    auto current = GetCurrentProcessorNumber();
    EXPECT_EQ(current, target);
#else
    auto current = static_cast<uint32_t>(sched_getcpu());
    EXPECT_EQ(current, target);
#endif
}

TEST(ThreadAffinityTest, PinToMultipleCoresSequentially)
{
    auto topo = discover_topology();
    // Test pinning to first two distinct physical cores
    for (size_t i = 0; i < std::min<size_t>(2, topo.physical_cores.size()); ++i)
    {
        auto target = topo.physical_cores[i].primary_logical_id();
        EXPECT_TRUE(apply_thread_affinity(target)) << "Failed to pin to logical core " << target;
    }
}

TEST(ThreadAffinityTest, NumaPolicyDoesNotCrash)
{
    // NUMA policy application should succeed (or no-op on Windows) without crashing
    EXPECT_TRUE(apply_numa_memory_policy(0));
}

TEST(ThreadAffinityTest, InvalidCoreFails)
{
    // Extremely large core ID should fail gracefully (no crash)
    auto result = apply_thread_affinity(99999);
    EXPECT_FALSE(result);
}
