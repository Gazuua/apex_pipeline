// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/scoped_logger.hpp>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace apex::core
{

/// Per-IP connection counter using the owner-shard pattern (Seastar-style).
///
/// Each core owns a ConnectionLimiter instance. An IP's counter lives on exactly
/// one core, determined by `owner_core(ip, num_cores)`. Callers on other cores
/// use `cross_core_call` / `cross_core_post` to reach the owner.
///
/// No mutex, no atomic, no CAS — single-thread access only (per-core io_context).
class ConnectionLimiter
{
  public:
    ConnectionLimiter(uint32_t core_id, uint32_t num_cores, uint32_t max_per_ip);

    /// Check and increment the counter for @p ip. Returns true if under the limit.
    /// Must be called on the owner core for this IP.
    [[nodiscard]] bool try_increment(std::string_view ip);

    /// Decrement the counter for @p ip. Removes the entry if count reaches 0.
    /// Must be called on the owner core for this IP.
    void decrement(std::string_view ip);

    /// Determine which core owns the counter for @p ip.
    [[nodiscard]] static uint32_t owner_core(std::string_view ip, uint32_t num_cores) noexcept;

    /// Current connection count for @p ip (0 if not tracked). For diagnostics only.
    [[nodiscard]] uint32_t count(std::string_view ip) const noexcept;

    /// Total number of tracked IPs on this core.
    [[nodiscard]] size_t tracked_ips() const noexcept;

  private:
    uint32_t core_id_;
    uint32_t num_cores_;
    uint32_t max_per_ip_;
    boost::unordered_flat_map<std::string, uint32_t> counts_;
    ScopedLogger logger_;
};

} // namespace apex::core
