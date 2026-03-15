#include <apex/gateway/route_table.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>

namespace apex::gateway {

apex::core::Result<RouteTable>
RouteTable::build(std::vector<RouteEntry> routes) {
    RouteTable table;

    for (auto& r : routes) {
        if (r.range_begin > r.range_end) {
            spdlog::error("Invalid route: begin({}) > end({})",
                r.range_begin, r.range_end);
            return apex::core::error(apex::core::ErrorCode::ConfigParseFailed);
        }
        table.entries_.push_back(Entry{
            .range_begin = r.range_begin,
            .range_end = r.range_end,
            .kafka_topic = std::move(r.kafka_topic),
        });
    }

    // Sort by range_end for binary search
    std::ranges::sort(table.entries_, [](const Entry& a, const Entry& b) {
        return a.range_end < b.range_end;
    });

    // Validate
    auto result = table.validate();
    if (!result) return apex::core::error(result.error());

    return table;
}

std::optional<std::string_view>
RouteTable::resolve(uint32_t msg_id) const noexcept {
    // Binary search: find first entry where range_end >= msg_id
    auto it = std::ranges::lower_bound(entries_, msg_id,
        std::less<>{}, &Entry::range_end);

    if (it != entries_.end() &&
        msg_id >= it->range_begin && msg_id <= it->range_end) {
        return it->kafka_topic;
    }
    return std::nullopt;
}

apex::core::Result<void> RouteTable::validate() const {
    for (size_t i = 1; i < entries_.size(); ++i) {
        // Overlap check: previous range_end >= current range_begin
        if (entries_[i - 1].range_end >= entries_[i].range_begin) {
            spdlog::error("Route overlap: [{}, {}] and [{}, {}]",
                entries_[i - 1].range_begin, entries_[i - 1].range_end,
                entries_[i].range_begin, entries_[i].range_end);
            return apex::core::error(apex::core::ErrorCode::ConfigParseFailed);
        }
    }
    return apex::core::ok();
}

} // namespace apex::gateway
