// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <cstdint>

namespace apex::core
{

/// Pin the calling thread to a specific logical CPU core.
/// @param logical_core_id  The OS-level logical processor ID.
/// @return true on success, false on failure (warning logged internally).
bool apply_thread_affinity(uint32_t logical_core_id);

/// Bind the calling thread's memory allocations to a specific NUMA node.
/// Linux: set_mempolicy(MPOL_BIND).  Windows: no-op (first-touch policy).
/// @param numa_node  The NUMA node ID.
/// @return true on success (or no-op), false on failure (warning logged internally).
bool apply_numa_memory_policy(uint32_t numa_node);

} // namespace apex::core
