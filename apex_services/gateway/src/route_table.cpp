// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/route_table.hpp>

#include <apex/gateway/gateway_error.hpp>

#include <apex/core/scoped_logger.hpp>

#include <algorithm>

namespace apex::gateway
{

namespace
{
const apex::core::ScopedLogger& s_logger()
{
    static const apex::core::ScopedLogger instance{"RouteTable", apex::core::ScopedLogger::NO_CORE, "app"};
    return instance;
}
} // anonymous namespace

apex::core::Result<RouteTable> RouteTable::build(std::vector<RouteEntry> routes)
{
    RouteTable table;

    for (const auto& r : routes)
    {
        if (r.range_begin > r.range_end)
        {
            s_logger().error("Invalid route: begin({}) > end({})", r.range_begin, r.range_end);
            return apex::core::error(apex::core::ErrorCode::ServiceError);
        }
    }

    table.entries_ = std::move(routes);

    // Sort by range_end for binary search
    std::ranges::sort(table.entries_,
                      [](const RouteEntry& a, const RouteEntry& b) { return a.range_end < b.range_end; });

    // Validate
    auto result = table.validate();
    if (!result)
        return apex::core::error(result.error());

    return table;
}

std::optional<std::string_view> RouteTable::resolve(uint32_t msg_id) const noexcept
{
    // Binary search: find first entry where range_end >= msg_id
    auto it = std::ranges::lower_bound(entries_, msg_id, std::less<>{}, &RouteEntry::range_end);

    if (it != entries_.end() && msg_id >= it->range_begin && msg_id <= it->range_end)
    {
        return it->kafka_topic;
    }
    return std::nullopt;
}

apex::core::Result<void> RouteTable::validate() const
{
    for (size_t i = 1; i < entries_.size(); ++i)
    {
        // Overlap check: previous range_end >= current range_begin
        if (entries_[i - 1].range_end >= entries_[i].range_begin)
        {
            s_logger().error("Route overlap: [{}, {}] and [{}, {}]", entries_[i - 1].range_begin,
                             entries_[i - 1].range_end, entries_[i].range_begin, entries_[i].range_end);
            return apex::core::error(apex::core::ErrorCode::ServiceError);
        }
    }
    return apex::core::ok();
}

} // namespace apex::gateway
