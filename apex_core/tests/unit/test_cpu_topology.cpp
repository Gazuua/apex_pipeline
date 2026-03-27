// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/cpu_topology.hpp>

#include <gtest/gtest.h>

#include <thread>

using namespace apex::core;

TEST(CpuTopologyTest, DiscoverReturnsNonEmpty)
{
    auto topo = discover_topology();
    EXPECT_GE(topo.physical_core_count(), 1u);
    EXPECT_GE(topo.logical_core_count(), topo.physical_core_count());
    EXPECT_GE(topo.numa_node_count, 1u);
}

TEST(CpuTopologyTest, LogicalCountAtLeastHardwareConcurrency)
{
    auto topo = discover_topology();
    auto hw = std::thread::hardware_concurrency();
    if (hw > 0)
    {
        // Logical core count should match hardware_concurrency
        EXPECT_EQ(topo.logical_core_count(), hw);
    }
}

TEST(CpuTopologyTest, EachPhysicalCoreHasAtLeastOneLogical)
{
    auto topo = discover_topology();
    for (const auto& pc : topo.physical_cores)
    {
        EXPECT_FALSE(pc.logical_ids.empty()) << "physical_id=" << pc.physical_id;
        EXPECT_EQ(pc.primary_logical_id(), pc.logical_ids.front());
    }
}

TEST(CpuTopologyTest, NumaNodeConsistency)
{
    auto topo = discover_topology();
    for (const auto& pc : topo.physical_cores)
    {
        EXPECT_LT(pc.numa_node, topo.numa_node_count) << "physical_id=" << pc.physical_id;
    }
}

TEST(CpuTopologyTest, CoresByNuma)
{
    auto topo = discover_topology();
    uint32_t total = 0;
    for (uint32_t node = 0; node < topo.numa_node_count; ++node)
    {
        auto cores = topo.cores_by_numa(node);
        total += static_cast<uint32_t>(cores.size());
    }
    EXPECT_EQ(total, topo.physical_core_count());
}

TEST(CpuTopologyTest, PerfAndEfficiencyCoresPartition)
{
    auto topo = discover_topology();
    auto p = topo.performance_cores();
    auto e = topo.efficiency_cores();
    // P + E + Unknown should equal total
    uint32_t classified = static_cast<uint32_t>(p.size() + e.size());
    EXPECT_LE(classified, topo.physical_core_count());
}

TEST(CpuTopologyTest, SummaryNotEmpty)
{
    auto topo = discover_topology();
    auto s = topo.summary();
    EXPECT_FALSE(s.empty());
}

TEST(CpuTopologyTest, ToStringCoreType)
{
    EXPECT_STREQ(to_string(CoreType::Performance), "Performance");
    EXPECT_STREQ(to_string(CoreType::Efficiency), "Efficiency");
    EXPECT_STREQ(to_string(CoreType::Unknown), "Unknown");
}
