// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace apex::core
{

/// CPU core type classification for hybrid architectures (Intel Alder Lake+).
enum class CoreType : uint8_t
{
    Performance, ///< P-Core (high frequency, full execution width)
    Efficiency,  ///< E-Core (lower frequency, narrower execution)
    Unknown      ///< Type could not be determined (non-hybrid or detection failed)
};

/// A physical CPU core with its HT siblings and NUMA affiliation.
struct PhysicalCore
{
    uint32_t physical_id;              ///< Physical core ID
    uint32_t numa_node{0};             ///< NUMA node (0 for single-socket)
    CoreType type{CoreType::Unknown};  ///< P/E/Unknown
    std::vector<uint32_t> logical_ids; ///< Logical (HT sibling) IDs

    /// First logical ID — used for thread affinity pinning (one worker per physical core).
    /// Fallback to physical_id if logical_ids is empty (should never happen in practice —
    /// discover_topology() always populates at least one logical ID per physical core).
    [[nodiscard]] uint32_t primary_logical_id() const noexcept
    {
        return logical_ids.empty() ? physical_id : logical_ids.front();
    }
};

/// System CPU topology — physical cores, NUMA nodes, P/E classification.
/// Immutable after construction.
struct CpuTopology
{
    std::vector<PhysicalCore> physical_cores;
    uint32_t numa_node_count{1};

    /// Physical cores belonging to a specific NUMA node.
    [[nodiscard]] std::vector<const PhysicalCore*> cores_by_numa(uint32_t node) const;

    /// Performance (P) cores only. Empty if no hybrid detection.
    [[nodiscard]] std::vector<const PhysicalCore*> performance_cores() const;

    /// Efficiency (E) cores only. Empty if no hybrid detection.
    [[nodiscard]] std::vector<const PhysicalCore*> efficiency_cores() const;

    /// Total physical core count.
    [[nodiscard]] uint32_t physical_core_count() const noexcept
    {
        return static_cast<uint32_t>(physical_cores.size());
    }

    /// Total logical core count (sum of all HT siblings).
    [[nodiscard]] uint32_t logical_core_count() const noexcept;

    /// Human-readable summary for logging.
    [[nodiscard]] std::string summary() const;
};

/// Detect system CPU topology. Platform-specific implementation.
/// On failure: returns a fallback topology with hardware_concurrency() Unknown cores,
/// single NUMA node. Logs a warning but never throws.
[[nodiscard]] CpuTopology discover_topology();

/// Convert CoreType to string.
[[nodiscard]] const char* to_string(CoreType type) noexcept;

} // namespace apex::core
