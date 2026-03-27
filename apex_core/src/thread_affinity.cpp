// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/scoped_logger.hpp>
#include <apex/core/thread_affinity.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
// set_mempolicy may not be available via glibc headers on all distros.
// Use syscall fallback.
#include <sys/syscall.h>
#include <unistd.h>

// MPOL_BIND = 2 (from linux/mempolicy.h, avoid pulling full kernel headers)
#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif
#endif

namespace apex::core
{

namespace
{

const ScopedLogger& affinity_logger()
{
    static const ScopedLogger instance{"ThreadAffinity", ScopedLogger::NO_CORE};
    return instance;
}

} // anonymous namespace

bool apply_thread_affinity(uint32_t logical_core_id)
{
#ifdef _WIN32
    // Always use SetThreadGroupAffinity for correctness on 64+ core systems.
    // SetThreadAffinityMask is processor-group-unaware and may pin to the wrong core
    // when the process spans multiple groups.
    GROUP_AFFINITY ga{};
    ga.Group = static_cast<WORD>(logical_core_id / 64);
    ga.Mask = 1ULL << (logical_core_id % 64);
    if (SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr) == 0)
    {
        affinity_logger().warn("SetThreadGroupAffinity failed for logical_core={}: error={}", logical_core_id,
                               GetLastError());
        return false;
    }
    return true;

#else
    // Linux: pthread_setaffinity_np
    // cpu_set_t supports up to CPU_SETSIZE (typically 1024) logical CPUs.
    if (logical_core_id >= CPU_SETSIZE)
    {
        affinity_logger().warn("logical_core_id {} exceeds CPU_SETSIZE ({})", logical_core_id, CPU_SETSIZE);
        return false;
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<int>(logical_core_id), &cpuset);

    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0)
    {
        affinity_logger().warn("pthread_setaffinity_np failed for logical_core={}: {}", logical_core_id,
                               std::strerror(rc));
        return false;
    }
    return true;
#endif
}

bool apply_numa_memory_policy(uint32_t numa_node)
{
#ifdef _WIN32
    // Windows: no direct set_mempolicy equivalent.
    // First-touch policy with thread affinity is sufficient.
    (void)numa_node;
    return true;

#else
    // Linux: set_mempolicy(MPOL_BIND, nodemask, maxnode)
    // Use syscall directly to avoid libnuma dependency.
    // nodemask is a bitmask of allowed NUMA nodes.
    constexpr size_t BITS_PER_ULONG = sizeof(unsigned long) * 8;
    constexpr size_t MAX_NODES = 256;
    unsigned long nodemask[MAX_NODES / BITS_PER_ULONG] = {};

    if (numa_node >= MAX_NODES)
    {
        affinity_logger().warn("NUMA node {} exceeds max supported ({})", numa_node, MAX_NODES);
        return false;
    }

    nodemask[numa_node / BITS_PER_ULONG] = 1UL << (numa_node % BITS_PER_ULONG);

    // maxnode is the range of node IDs (kernel uses maxnode-1 as the highest bit index).
    // Pass MAX_NODES + 1 so node IDs 0..MAX_NODES-1 are all representable.
    long rc = syscall(SYS_set_mempolicy, MPOL_BIND, nodemask, MAX_NODES + 1);
    if (rc != 0)
    {
        affinity_logger().warn("set_mempolicy(MPOL_BIND) failed for NUMA node {}: {}", numa_node, std::strerror(errno));
        return false;
    }
    return true;
#endif
}

} // namespace apex::core
