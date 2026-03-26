// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/connection_limiter.hpp>

namespace apex::core
{

ConnectionLimiter::ConnectionLimiter(uint32_t core_id, uint32_t num_cores, uint32_t max_per_ip)
    : core_id_(core_id)
    , num_cores_(num_cores)
    , max_per_ip_(max_per_ip)
    , logger_("ConnLimiter", core_id, "core")
{}

bool ConnectionLimiter::try_increment(std::string_view ip)
{
    auto [it, inserted] = counts_.try_emplace(std::string(ip), 0);
    if (it->second >= max_per_ip_)
    {
        logger_.warn("per-IP limit reached: ip={}, count={}, max={}", ip, it->second, max_per_ip_);
        if (inserted)
            counts_.erase(it);
        return false;
    }
    ++it->second;
    return true;
}

void ConnectionLimiter::decrement(std::string_view ip)
{
    auto it = counts_.find(std::string(ip));
    if (it == counts_.end())
    {
        logger_.debug("decrement for unknown ip={} (already cleaned up)", ip);
        return;
    }
    if (--it->second == 0)
        counts_.erase(it);
}

uint32_t ConnectionLimiter::owner_core(std::string_view ip, uint32_t num_cores) noexcept
{
    // FNV-1a hash — deterministic, fast, good distribution
    uint64_t hash = 14695981039346656037ULL;
    for (auto c : ip)
    {
        hash ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
        hash *= 1099511628211ULL;
    }
    return static_cast<uint32_t>(hash % num_cores);
}

uint32_t ConnectionLimiter::count(std::string_view ip) const noexcept
{
    auto it = counts_.find(std::string(ip));
    return it != counts_.end() ? it->second : 0;
}

size_t ConnectionLimiter::tracked_ips() const noexcept
{
    return counts_.size();
}

} // namespace apex::core
