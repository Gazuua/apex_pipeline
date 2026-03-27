// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/core_engine.hpp>
#include <apex/core/cpu_topology.hpp>
#include <apex/core/thread_affinity.hpp>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace apex::bench
{

/// Global affinity state — set by init_affinity(), read by benchmark functions.
inline bool g_affinity_enabled = false;
inline std::vector<apex::core::CoreAssignment> g_core_assignments;

/// Parse --affinity=on|off from argv, strip it, populate globals.
/// Call before benchmark::Initialize().
inline void init_affinity(int& argc, char** argv)
{
    bool affinity_on = false;
    int write_idx = 1;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], "--affinity=", 11) == 0)
        {
            std::string val(argv[i] + 11);
            affinity_on = (val == "on" || val == "true" || val == "1");
            continue;
        }
        argv[write_idx++] = argv[i];
    }
    argc = write_idx;

    if (!affinity_on)
    {
        std::cout << "Affinity: OFF (default)\n";
        return;
    }

    auto topology = apex::core::discover_topology();
    g_core_assignments.reserve(topology.physical_core_count());
    for (const auto& pc : topology.physical_cores)
    {
        g_core_assignments.push_back({.logical_core_id = pc.primary_logical_id(), .numa_node = pc.numa_node});
    }
    g_affinity_enabled = true;
    std::cout << "Affinity: ON (" << g_core_assignments.size() << " physical cores)\n";
}

/// Build CoreAssignment vector for N cores (used by CoreEngine benchmarks).
/// When affinity is enabled, maps to discovered physical cores (round-robin if N > physical).
/// When disabled, returns empty vector (CoreEngine legacy behavior).
inline std::vector<apex::core::CoreAssignment> build_assignments(uint32_t num_cores)
{
    if (!g_affinity_enabled)
        return {};

    std::vector<apex::core::CoreAssignment> result;
    result.reserve(num_cores);
    for (uint32_t i = 0; i < num_cores; ++i)
    {
        result.push_back(g_core_assignments[i % g_core_assignments.size()]);
    }
    return result;
}

/// Apply thread affinity for raw-thread benchmarks (not CoreEngine).
/// worker_index is mapped to physical core via round-robin.
inline void apply_worker_affinity(int worker_index)
{
    if (!g_affinity_enabled || g_core_assignments.empty())
        return;
    auto idx = static_cast<size_t>(worker_index) % g_core_assignments.size();
    (void)apex::core::apply_thread_affinity(g_core_assignments[idx].logical_core_id);
}

} // namespace apex::bench
