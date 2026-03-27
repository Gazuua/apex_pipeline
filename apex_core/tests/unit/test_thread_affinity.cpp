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

/// Fixture that restores thread affinity to all cores after each test,
/// preventing one test's pinning from affecting subsequent tests.
class ThreadAffinityTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
#ifdef _WIN32
        // Restore affinity to all processors in group 0
        GROUP_AFFINITY ga{};
        ga.Group = 0;
        ga.Mask = ~KAFFINITY{0}; // all bits set
        SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr);
#else
        // Restore affinity to all CPUs
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        auto hw = std::thread::hardware_concurrency();
        for (unsigned i = 0; i < (hw > 0 ? hw : 1); ++i)
            CPU_SET(static_cast<int>(i), &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
    }
};

TEST_F(ThreadAffinityTest, PinToFirstLogicalCore)
{
    auto topo = discover_topology();
    ASSERT_FALSE(topo.physical_cores.empty());

    auto target = topo.physical_cores.front().primary_logical_id();
    EXPECT_TRUE(apply_thread_affinity(target));

    // After affinity is set, yield to let the OS migrate if needed,
    // then verify the current processor matches the target.
#ifdef _WIN32
    SwitchToThread();
    auto current = GetCurrentProcessorNumber();
    EXPECT_EQ(current, target);
#else
    sched_yield();
    auto current = static_cast<uint32_t>(sched_getcpu());
    EXPECT_EQ(current, target);
#endif
}

TEST_F(ThreadAffinityTest, PinToMultipleCoresSequentially)
{
    auto topo = discover_topology();
    // Test pinning to first two distinct physical cores
    for (size_t i = 0; i < std::min<size_t>(2, topo.physical_cores.size()); ++i)
    {
        auto target = topo.physical_cores[i].primary_logical_id();
        EXPECT_TRUE(apply_thread_affinity(target)) << "Failed to pin to logical core " << target;
    }
}

TEST_F(ThreadAffinityTest, NumaPolicyNodeZeroDoesNotCrash)
{
    // NUMA node 0 should succeed on bare metal, but may fail in containers
    // (set_mempolicy requires CAP_SYS_NICE or NUMA-enabled kernel).
    // Windows: always true (no-op). Linux: environment-dependent.
    // Verify no crash regardless of result.
    [[maybe_unused]] auto result = apply_numa_memory_policy(0);
#ifdef _WIN32
    EXPECT_TRUE(result);
#endif
}

TEST_F(ThreadAffinityTest, NumaPolicyInvalidNodeDoesNotCrash)
{
    // Non-existent NUMA node — verify graceful failure, no crash.
    // Windows: always true (no-op). Linux: should fail but environment-dependent.
    [[maybe_unused]] auto result = apply_numa_memory_policy(255);
#ifdef _WIN32
    EXPECT_TRUE(result);
#endif
}

TEST_F(ThreadAffinityTest, InvalidCoreFails)
{
    // Extremely large core ID should fail gracefully (no crash)
    auto result = apply_thread_affinity(99999);
    EXPECT_FALSE(result);
}
