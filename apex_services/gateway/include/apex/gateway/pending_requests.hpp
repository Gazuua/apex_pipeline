// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/core/session.hpp>

#include <boost/unordered/unordered_flat_map.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace apex::gateway
{

/// per-core Pending Requests Map.
/// Correlation ID -> Session matching.
/// Timeout is sweep-based.
class PendingRequestsMap
{
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using NowFn = std::function<TimePoint()>;

    struct PendingEntry
    {
        apex::core::SessionId session_id;
        uint32_t original_msg_id; // msg_id to restore in response WireHeader
        TimePoint deadline;
    };

    /// @param max_entries per-core max pending count
    /// @param timeout Default request timeout
    /// @param now_fn Time source (default: steady_clock::now). Inject for testing.
    explicit PendingRequestsMap(size_t max_entries = 65536,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds{5000},
                                NowFn now_fn = Clock::now);

    /// Register new pending request.
    /// @return Success or PendingMapFull
    [[nodiscard]] apex::core::Result<void> insert(uint64_t corr_id, apex::core::SessionId session_id,
                                                  uint32_t original_msg_id);

    /// Extract pending request by correlation ID (one-shot).
    [[nodiscard]] std::optional<PendingEntry> extract(uint64_t corr_id);

    /// Sweep expired requests.
    /// @param callback Called for each expired request
    void sweep_expired(std::function<void(uint64_t corr_id, const PendingEntry&)> callback);

    /// Current pending count.
    [[nodiscard]] size_t size() const noexcept
    {
        return map_.size();
    }

  private:
    boost::unordered_flat_map<uint64_t, PendingEntry> map_;
    size_t max_entries_;
    std::chrono::milliseconds timeout_;
    NowFn now_fn_;
};

} // namespace apex::gateway
