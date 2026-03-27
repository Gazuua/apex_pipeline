// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/cpu_topology.hpp>
#include <apex/core/scoped_logger.hpp>

#include <algorithm>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#endif

namespace apex::core
{

namespace
{

const ScopedLogger& topo_logger()
{
    static const ScopedLogger instance{"CpuTopology", ScopedLogger::NO_CORE};
    return instance;
}

/// Fallback topology: N Unknown cores, single NUMA node, 1:1 logical mapping.
CpuTopology make_fallback_topology()
{
    auto hw = std::thread::hardware_concurrency();
    uint32_t count = (hw > 0) ? hw : 1;

    CpuTopology topo;
    topo.numa_node_count = 1;
    topo.physical_cores.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        PhysicalCore pc;
        pc.physical_id = i;
        pc.numa_node = 0;
        pc.type = CoreType::Unknown;
        pc.logical_ids = {i};
        topo.physical_cores.push_back(std::move(pc));
    }
    return topo;
}

#ifdef _WIN32

CpuTopology discover_topology_impl()
{
    CpuTopology topo;

    // --- Phase 1: Discover physical cores via RelationProcessorCore ---
    DWORD core_buf_len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &core_buf_len);
    if (core_buf_len == 0)
    {
        topo_logger().warn("GetLogicalProcessorInformationEx(RelationProcessorCore) failed: size=0");
        return make_fallback_topology();
    }

    std::vector<uint8_t> core_buf(core_buf_len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(core_buf.data()),
                                          &core_buf_len))
    {
        topo_logger().warn("GetLogicalProcessorInformationEx(RelationProcessorCore) failed: error={}", GetLastError());
        return make_fallback_topology();
    }

    // Determine max EfficiencyClass to distinguish P/E on hybrid CPUs.
    // Non-hybrid: all cores have the same class (all Performance).
    uint8_t max_efficiency_class = 0;
    {
        auto* ptr = core_buf.data();
        auto* end = ptr + core_buf_len;
        while (ptr < end)
        {
            auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
            if (info->Relationship == RelationProcessorCore)
            {
                if (info->Processor.EfficiencyClass > max_efficiency_class)
                    max_efficiency_class = info->Processor.EfficiencyClass;
            }
            ptr += info->Size;
        }
    }

    uint32_t phys_id = 0;
    {
        auto* ptr = core_buf.data();
        auto* end = ptr + core_buf_len;
        while (ptr < end)
        {
            auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
            if (info->Relationship == RelationProcessorCore)
            {
                PhysicalCore pc;
                pc.physical_id = phys_id++;

                // P/E classification
                if (max_efficiency_class > 0)
                {
                    // Hybrid CPU: max class = P-Core, lower = E-Core
                    pc.type = (info->Processor.EfficiencyClass == max_efficiency_class) ? CoreType::Performance
                                                                                        : CoreType::Efficiency;
                }
                else
                {
                    // Non-hybrid: all cores are uniform
                    pc.type = CoreType::Unknown;
                }

                // Extract logical core IDs from group mask
                for (WORD g = 0; g < info->Processor.GroupCount; ++g)
                {
                    auto mask = info->Processor.GroupMask[g].Mask;
                    auto group = info->Processor.GroupMask[g].Group;
                    uint32_t bit = 0;
                    while (mask != 0)
                    {
                        if (mask & 1)
                        {
                            pc.logical_ids.push_back(static_cast<uint32_t>(group) * 64 + bit);
                        }
                        mask >>= 1;
                        ++bit;
                    }
                }

                topo.physical_cores.push_back(std::move(pc));
            }
            ptr += info->Size;
        }
    }

    // --- Phase 2: NUMA node discovery via RelationNumaNode ---
    DWORD numa_buf_len = 0;
    GetLogicalProcessorInformationEx(RelationNumaNode, nullptr, &numa_buf_len);
    if (numa_buf_len > 0)
    {
        std::vector<uint8_t> numa_buf(numa_buf_len);
        if (GetLogicalProcessorInformationEx(
                RelationNumaNode, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(numa_buf.data()),
                &numa_buf_len))
        {
            // Build logical_id → NUMA node mapping
            std::unordered_map<uint32_t, uint32_t> logical_to_numa;
            uint32_t max_numa = 0;

            auto* ptr = numa_buf.data();
            auto* end = ptr + numa_buf_len;
            while (ptr < end)
            {
                auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
                if (info->Relationship == RelationNumaNode)
                {
                    uint32_t node = info->NumaNode.NodeNumber;
                    if (node > max_numa)
                        max_numa = node;

                    auto mask = info->NumaNode.GroupMask.Mask;
                    auto group = info->NumaNode.GroupMask.Group;
                    uint32_t bit = 0;
                    while (mask != 0)
                    {
                        if (mask & 1)
                        {
                            logical_to_numa[static_cast<uint32_t>(group) * 64 + bit] = node;
                        }
                        mask >>= 1;
                        ++bit;
                    }
                }
                ptr += info->Size;
            }

            topo.numa_node_count = max_numa + 1;

            // Apply NUMA node to physical cores
            for (auto& pc : topo.physical_cores)
            {
                if (!pc.logical_ids.empty())
                {
                    auto it = logical_to_numa.find(pc.logical_ids.front());
                    if (it != logical_to_numa.end())
                    {
                        pc.numa_node = it->second;
                    }
                }
            }
        }
    }

    if (topo.physical_cores.empty())
    {
        topo_logger().warn("no physical cores detected, using fallback");
        return make_fallback_topology();
    }

    return topo;
}

#else // Linux

CpuTopology discover_topology_impl()
{
    CpuTopology topo;

    // Build physical core map: (package_id, core_id) → PhysicalCore
    // Each logical CPU has topology/physical_package_id and topology/core_id
    struct CoreKey
    {
        uint32_t package_id;
        uint32_t core_id;
        bool operator==(const CoreKey&) const = default;
    };
    struct CoreKeyHash
    {
        size_t operator()(const CoreKey& k) const
        {
            return std::hash<uint64_t>{}(static_cast<uint64_t>(k.package_id) << 32 | k.core_id);
        }
    };

    std::unordered_map<CoreKey, size_t, CoreKeyHash> core_map; // key → index in physical_cores

    // Detect number of logical CPUs
    auto hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 1;

    for (uint32_t cpu = 0; cpu < hw; ++cpu)
    {
        std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);

        // Check online status
        {
            std::ifstream f(base + "/online");
            if (f.is_open())
            {
                int online = 1;
                f >> online;
                if (online == 0)
                    continue;
            }
            // cpu0 may not have 'online' file — always online
        }

        // Read topology
        uint32_t package_id = 0;
        uint32_t core_id = 0;

        {
            std::ifstream f(base + "/topology/physical_package_id");
            if (!f.is_open() || !(f >> package_id))
                continue; // skip if no topology info or unreadable
        }
        {
            std::ifstream f(base + "/topology/core_id");
            if (!f.is_open() || !(f >> core_id))
                continue;
        }

        CoreKey key{package_id, core_id};
        auto it = core_map.find(key);
        if (it == core_map.end())
        {
            PhysicalCore pc;
            pc.physical_id = static_cast<uint32_t>(topo.physical_cores.size());
            pc.logical_ids.push_back(cpu);

            // NUMA node is resolved below via /sys/devices/system/node/nodeN/cpulist

            core_map[key] = topo.physical_cores.size();
            topo.physical_cores.push_back(std::move(pc));
        }
        else
        {
            // HT sibling — add logical ID to existing physical core
            topo.physical_cores[it->second].logical_ids.push_back(cpu);
        }
    }

    // --- NUMA node mapping ---
    // Read /sys/devices/system/node/ to find NUMA nodes and their CPUs
    {
        std::set<uint32_t> numa_nodes;
        for (uint32_t node = 0; node < 256; ++node) // reasonable upper bound
        {
            std::string path = "/sys/devices/system/node/node" + std::to_string(node) + "/cpulist";
            std::ifstream f(path);
            if (!f.is_open())
                continue; // NUMA nodes may be non-contiguous (hotplug, offline, BIOS config)

            numa_nodes.insert(node);

            // Parse cpulist format: "0-7,16-23"
            std::string cpulist;
            std::getline(f, cpulist);

            std::set<uint32_t> cpus_in_node;
            try
            {
                std::istringstream iss(cpulist);
                std::string range;
                while (std::getline(iss, range, ','))
                {
                    if (range.empty())
                        continue;
                    auto dash = range.find('-');
                    if (dash != std::string::npos)
                    {
                        uint32_t lo = static_cast<uint32_t>(std::stoul(range.substr(0, dash)));
                        uint32_t hi = static_cast<uint32_t>(std::stoul(range.substr(dash + 1)));
                        for (uint32_t c = lo; c <= hi; ++c)
                            cpus_in_node.insert(c);
                    }
                    else
                    {
                        cpus_in_node.insert(static_cast<uint32_t>(std::stoul(range)));
                    }
                }
            }
            catch (const std::exception& e)
            {
                topo_logger().warn("failed to parse cpulist for NUMA node {}: {}", node, e.what());
                continue; // skip this node, keep other nodes' mapping
            }

            // Map physical cores to NUMA node
            for (auto& pc : topo.physical_cores)
            {
                for (auto lid : pc.logical_ids)
                {
                    if (cpus_in_node.count(lid))
                    {
                        pc.numa_node = node;
                        break;
                    }
                }
            }
        }

        topo.numa_node_count = numa_nodes.empty() ? 1 : static_cast<uint32_t>(numa_nodes.size());
    }

    // --- P/E core detection (kernel 5.7+ cpu_capacity) ---
    {
        uint32_t max_cap = 0;
        std::unordered_map<uint32_t, uint32_t> logical_capacity;

        for (uint32_t cpu = 0; cpu < hw; ++cpu)
        {
            std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpu_capacity";
            std::ifstream f(path);
            if (!f.is_open())
                break; // kernel doesn't support cpu_capacity

            uint32_t cap = 0;
            f >> cap;
            logical_capacity[cpu] = cap;
            if (cap > max_cap)
                max_cap = cap;
        }

        if (!logical_capacity.empty() && max_cap > 0)
        {
            // Classify: max capacity = Performance, lower = Efficiency
            bool has_variation = false;
            for (auto& [cpu, cap] : logical_capacity)
            {
                if (cap != max_cap)
                {
                    has_variation = true;
                    break;
                }
            }

            if (has_variation)
            {
                for (auto& pc : topo.physical_cores)
                {
                    if (!pc.logical_ids.empty())
                    {
                        auto it = logical_capacity.find(pc.logical_ids.front());
                        if (it != logical_capacity.end())
                        {
                            pc.type = (it->second == max_cap) ? CoreType::Performance : CoreType::Efficiency;
                        }
                    }
                }
            }
        }
    }

    if (topo.physical_cores.empty())
    {
        topo_logger().warn("no physical cores detected via sysfs, using fallback");
        return make_fallback_topology();
    }

    return topo;
}

#endif // _WIN32 / Linux

} // anonymous namespace

// --- CpuTopology methods ---

std::vector<const PhysicalCore*> CpuTopology::cores_by_numa(uint32_t node) const
{
    std::vector<const PhysicalCore*> result;
    for (const auto& pc : physical_cores)
    {
        if (pc.numa_node == node)
            result.push_back(&pc);
    }
    return result;
}

std::vector<const PhysicalCore*> CpuTopology::performance_cores() const
{
    std::vector<const PhysicalCore*> result;
    for (const auto& pc : physical_cores)
    {
        if (pc.type == CoreType::Performance)
            result.push_back(&pc);
    }
    return result;
}

std::vector<const PhysicalCore*> CpuTopology::efficiency_cores() const
{
    std::vector<const PhysicalCore*> result;
    for (const auto& pc : physical_cores)
    {
        if (pc.type == CoreType::Efficiency)
            result.push_back(&pc);
    }
    return result;
}

uint32_t CpuTopology::logical_core_count() const noexcept
{
    uint32_t total = 0;
    for (const auto& pc : physical_cores)
    {
        total += static_cast<uint32_t>(pc.logical_ids.size());
    }
    return total;
}

std::string CpuTopology::summary() const
{
    auto p_count = performance_cores().size();
    auto e_count = efficiency_cores().size();
    auto phys = physical_core_count();
    auto logical = logical_core_count();

    std::string result = std::to_string(phys) + " physical cores";
    if (p_count > 0 || e_count > 0)
    {
        result += " (" + std::to_string(p_count) + "P+" + std::to_string(e_count) + "E)";
    }
    result += ", " + std::to_string(logical) + " logical";
    result += ", " + std::to_string(numa_node_count) + " NUMA node" + (numa_node_count > 1 ? "s" : "");
    return result;
}

CpuTopology discover_topology()
{
    try
    {
        auto topo = discover_topology_impl();
        topo_logger().info("CPU topology: {}", topo.summary());
        return topo;
    }
    catch (const std::exception& e)
    {
        topo_logger().warn("topology detection failed: {}. Using fallback.", e.what());
        return make_fallback_topology();
    }
    catch (...)
    {
        topo_logger().warn("topology detection failed (unknown error). Using fallback.");
        return make_fallback_topology();
    }
}

const char* to_string(CoreType type) noexcept
{
    switch (type)
    {
        case CoreType::Performance:
            return "Performance";
        case CoreType::Efficiency:
            return "Efficiency";
        case CoreType::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

} // namespace apex::core
