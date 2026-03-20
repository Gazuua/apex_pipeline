// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/result.hpp>
#include <apex/gateway/gateway_config.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace apex::gateway
{

/// msg_id range-based routing table.
/// Binary search for O(log N) lookup.
/// Wrapped in shared_ptr for atomic replacement (hot-reload).
class RouteTable
{
  public:
    struct Entry
    {
        uint32_t range_begin;
        uint32_t range_end; // inclusive
        std::string kafka_topic;
    };

    /// Build route table from entries.
    /// Validates range overlap/gaps.
    /// @return Valid RouteTable or error
    [[nodiscard]] static apex::core::Result<RouteTable> build(std::vector<RouteEntry> routes);

    /// Resolve msg_id to Kafka topic.
    /// @return Topic name or nullopt (no routing rule)
    [[nodiscard]] std::optional<std::string_view> resolve(uint32_t msg_id) const noexcept;

    /// Entry count.
    [[nodiscard]] size_t size() const noexcept
    {
        return entries_.size();
    }

    /// Validation -- overlap/gap check.
    [[nodiscard]] apex::core::Result<void> validate() const;

  private:
    std::vector<Entry> entries_; // sorted by range_end
};

/// Shared pointer wrapper for atomic replacement.
/// On hot-reload, create new RouteTable and atomic store the pointer.
using RouteTablePtr = std::shared_ptr<const RouteTable>;

} // namespace apex::gateway
